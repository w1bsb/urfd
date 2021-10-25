//
//  cdextraprotocol.cpp
//  xlxd
//
//  Created by Jean-Luc Deltombe (LX3JL) on 01/11/2015.
//  Copyright © 2015 Jean-Luc Deltombe (LX3JL). All rights reserved.
//  Copyright © 2020 Thomas A. Early, N7TAE
//
// ----------------------------------------------------------------------------
//    This file is part of xlxd.
//
//    xlxd is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    xlxd is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with Foobar.  If not, see <http://www.gnu.org/licenses/>.
// ----------------------------------------------------------------------------

#include "Main.h"
#include <string.h>
#include "DExtraPeer.h"
#include "DExtraClient.h"
#include "DExtraProtocol.h"
#include "Reflector.h"
#include "GateKeeper.h"


////////////////////////////////////////////////////////////////////////////////////////
// operation

bool CDextraProtocol::Initialize(const char *type, int ptype, const uint16 port, const bool has_ipv4, const bool has_ipv6)
{
	// base class
	if (! CProtocol::Initialize(type, ptype, port, has_ipv4, has_ipv6))
		return false;

	// update time
	m_LastKeepaliveTime.Now();
	m_LastPeersLinkTime.Now();

	// done
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////
// task

void CDextraProtocol::Task(void)
{
	CBuffer             Buffer;
	CIp                 Ip;
	CCallsign           Callsign;
	char                ToLinkModule;
	int                 ProtRev;
	std::unique_ptr<CDvHeaderPacket>    Header;
	std::unique_ptr<CDvFramePacket>     Frame;
	std::unique_ptr<CDvLastFramePacket> LastFrame;

	// any incoming packet ?
#if DSTAR_IPV6==true
#if DSTAR_IPV4==true
	if ( ReceiveDS(Buffer, Ip, 20) )
#else
	if ( Receive6(Buffer, Ip, 20) )
#endif
#else
	if ( Receive4(Buffer, Ip, 20) )
#endif
	{
		// crack the packet
		if ( IsValidDvFramePacket(Buffer, Frame) )
		{
			OnDvFramePacketIn(Frame, &Ip);
		}
		else if ( IsValidDvHeaderPacket(Buffer, Header) )
		{
			// callsign muted?
			if ( g_GateKeeper.MayTransmit(Header->GetMyCallsign(), Ip, PROTOCOL_DEXTRA, Header->GetRpt2Module()) )
			{
				OnDvHeaderPacketIn(Header, Ip);
			}
		}
		else if ( IsValidDvLastFramePacket(Buffer, LastFrame) )
		{
			OnDvLastFramePacketIn(LastFrame, &Ip);
		}
		else if ( IsValidConnectPacket(Buffer, &Callsign, &ToLinkModule, &ProtRev) )
		{
			std::cout << "DExtra connect packet for module " << ToLinkModule << " from " << Callsign << " at " << Ip << " rev " << ProtRev << std::endl;

			// callsign authorized?
			if ( g_GateKeeper.MayLink(Callsign, Ip, PROTOCOL_DEXTRA) )
			{
				// valid module ?
				if ( g_Reflector.IsValidModule(ToLinkModule) )
				{
					// is this an ack for a link request?
					CPeerCallsignList *list = g_GateKeeper.GetPeerList();
					CCallsignListItem *item = list->FindListItem(Callsign);
					if ( item != nullptr && Callsign.GetModule() == item->GetModules()[1] && ToLinkModule == item->GetModules()[0] )
					{
						std::cout << "DExtra ack packet for module " << ToLinkModule << " from " << Callsign << " at " << Ip << std::endl;

						// already connected ?
						CPeers *peers = g_Reflector.GetPeers();
						if ( peers->FindPeer(Callsign, Ip, PROTOCOL_DEXTRA) == nullptr )
						{
							// create the new peer
							// this also create one client per module
							// append the peer to reflector peer list
							// this also add all new clients to reflector client list
							peers->AddPeer(std::make_shared<CDextraPeer>(Callsign, Ip, std::string(1, ToLinkModule).c_str(), CVersion(2, 0, 0)));
						}
						g_Reflector.ReleasePeers();
					}
					else
					{
						// acknowledge the request
						EncodeConnectAckPacket(&Buffer, ProtRev);
						Send(Buffer, Ip);

						// create the client and append
						g_Reflector.GetClients()->AddClient(std::make_shared<CDextraClient>(Callsign, Ip, ToLinkModule, ProtRev));
						g_Reflector.ReleaseClients();
					}
					g_GateKeeper.ReleasePeerList();
				}
				else
				{
					std::cout << "DExtra node " << Callsign << " connect attempt on non-existing module" << std::endl;

					// deny the request
					EncodeConnectNackPacket(&Buffer);
					Send(Buffer, Ip);
				}
			}
			else
			{
				// deny the request
				EncodeConnectNackPacket(&Buffer);
				Send(Buffer, Ip);
			}
		}
		else if ( IsValidDisconnectPacket(Buffer, &Callsign) )
		{
			std::cout << "DExtra disconnect packet from " << Callsign << " at " << Ip << std::endl;

			// find client & remove it
			CClients *clients = g_Reflector.GetClients();
			std::shared_ptr<CClient>client = clients->FindClient(Ip, PROTOCOL_DEXTRA);
			if ( client != nullptr )
			{
				// ack disconnect packet
				if ( client->GetProtocolRevision() == 1 )
				{
					EncodeDisconnectedPacket(&Buffer);
					Send(Buffer, Ip);
				}
				else if ( client->GetProtocolRevision() == 2 )
				{
					Send(Buffer, Ip);
				}
				// and remove it
				clients->RemoveClient(client);
			}
			g_Reflector.ReleaseClients();
		}
		else if ( IsValidKeepAlivePacket(Buffer, &Callsign) )
		{
			//std::cout << "DExtra keepalive packet from " << Callsign << " at " << Ip << std::endl;

			// find all clients with that callsign & ip and keep them alive
			CClients *clients = g_Reflector.GetClients();
			auto it = clients->begin();
			std::shared_ptr<CClient>client = nullptr;
			while ( (client = clients->FindNextClient(Callsign, Ip, PROTOCOL_DEXTRA, it)) != nullptr )
			{
				client->Alive();
			}
			g_Reflector.ReleaseClients();
		}
		else
		{
			std::string title("Unknown DExtra packet from ");
			title += Ip.GetAddress();
			Buffer.Dump(title);
		}
	}

	// handle end of streaming timeout
	CheckStreamsTimeout();

	// handle queue from reflector
	HandleQueue();

	// keep alive
	if ( m_LastKeepaliveTime.DurationSinceNow() > DEXTRA_KEEPALIVE_PERIOD )
	{
		// handle keep alives
		HandleKeepalives();

		// update time
		m_LastKeepaliveTime.Now();
	}

	// peer connections
	if ( m_LastPeersLinkTime.DurationSinceNow() > DEXTRA_RECONNECT_PERIOD )
	{
		// handle remote peers connections
		HandlePeerLinks();

		// update time
		m_LastPeersLinkTime.Now();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// queue helper

void CDextraProtocol::HandleQueue(void)
{
	m_Queue.Lock();
	while ( !m_Queue.empty() )
	{
		// get the packet
		auto packet = m_Queue.front();
		m_Queue.pop();

		// encode it
		CBuffer buffer;
		if ( EncodeDvPacket(*packet, &buffer) )
		{
			// and push it to all our clients linked to the module and who are not streaming in
			CClients *clients = g_Reflector.GetClients();
			auto it = clients->begin();
			std::shared_ptr<CClient>client = nullptr;
			while ( (client = clients->FindNextClient(PROTOCOL_DEXTRA, it)) != nullptr )
			{
				// is this client busy ?
				if ( !client->IsAMaster() && (client->GetReflectorModule() == packet->GetModuleId()) )
				{
					// no, send the packet
					int n = packet->IsDvHeader() ? 5 : 1;
					for ( int i = 0; i < n; i++ )
					{
						Send(buffer, client->GetIp());
					}
				}
			}
			g_Reflector.ReleaseClients();
		}
	}
	m_Queue.Unlock();
}

////////////////////////////////////////////////////////////////////////////////////////
// keepalive helpers

void CDextraProtocol::HandleKeepalives(void)
{
	// DExtra protocol sends and monitors keepalives packets
	// event if the client is currently streaming
	// so, send keepalives to all
	CBuffer keepalive;
	EncodeKeepAlivePacket(&keepalive);

	// iterate on clients
	CClients *clients = g_Reflector.GetClients();
	auto it = clients->begin();
	std::shared_ptr<CClient>client = nullptr;
	while ( (client = clients->FindNextClient(PROTOCOL_DEXTRA, it)) != nullptr )
	{
		// send keepalive
		Send(keepalive, client->GetIp());

		// client busy ?
		if ( client->IsAMaster() )
		{
			// yes, just tickle it
			client->Alive();
		}
		// otherwise check if still with us
		else if ( !client->IsAlive() )
		{
			CPeers *peers = g_Reflector.GetPeers();
			std::shared_ptr<CPeer>peer = peers->FindPeer(client->GetCallsign(), client->GetIp(), PROTOCOL_DEXTRA);
			if ( peer != nullptr && peer->GetReflectorModules()[0] == client->GetReflectorModule() )
			{
				// no, but this is a peer client, so it will be handled below
			}
			else
			{
				// no, disconnect
				CBuffer disconnect;
				EncodeDisconnectPacket(&disconnect, client->GetReflectorModule());
				Send(disconnect, client->GetIp());

				// remove it
				std::cout << "DExtra client " << client->GetCallsign() << " keepalive timeout" << std::endl;
				clients->RemoveClient(client);
			}
			g_Reflector.ReleasePeers();
		}

	}
	g_Reflector.ReleaseClients();

	// iterate on peers
	CPeers *peers = g_Reflector.GetPeers();
	auto pit = peers->begin();
	std::shared_ptr<CPeer>peer = nullptr;
	while ( (peer = peers->FindNextPeer(PROTOCOL_DEXTRA, pit)) != nullptr )
	{
		// keepalives are sent between clients

		// some client busy or still with us ?
		if ( !peer->IsAMaster() && !peer->IsAlive() )
		{
			// no, disconnect all clients
			CBuffer disconnect;
			EncodeDisconnectPacket(&disconnect, peer->GetReflectorModules()[0]);
			CClients *clients = g_Reflector.GetClients();
			for ( auto cit=peer->cbegin(); cit!=peer->cend(); cit++ )
			{
				Send(disconnect, (*cit)->GetIp());
			}
			g_Reflector.ReleaseClients();

			// remove it
			std::cout << "DExtra peer " << peer->GetCallsign() << " keepalive timeout" << std::endl;
			peers->RemovePeer(peer);
		}
	}
	g_Reflector.ReleasePeers();
}

////////////////////////////////////////////////////////////////////////////////////////
// Peers helpers

void CDextraProtocol::HandlePeerLinks(void)
{
	CBuffer buffer;

	// get the list of peers
	CPeerCallsignList *list = g_GateKeeper.GetPeerList();
	CPeers *peers = g_Reflector.GetPeers();

	// check if all our connected peers are still listed by gatekeeper
	// if not, disconnect
	auto pit = peers->begin();
	std::shared_ptr<CPeer>peer = nullptr;
	while ( (peer = peers->FindNextPeer(PROTOCOL_DEXTRA, pit)) != nullptr )
	{
		if ( list->FindListItem(peer->GetCallsign()) == nullptr )
		{
			// send disconnect packet
			EncodeDisconnectPacket(&buffer, peer->GetReflectorModules()[0]);
			Send(buffer, peer->GetIp());
			std::cout << "Sending disconnect packet to XRF peer " << peer->GetCallsign() << " at " << peer->GetIp() << std::endl;
			// remove client
			peers->RemovePeer(peer);
		}
	}

	// check if all ours peers listed by gatekeeper are connected
	// if not, connect or reconnect
	for ( auto it=list->begin(); it!=list->end(); it++ )
	{
		if ( !(*it).GetCallsign().HasSameCallsignWithWildcard(CCallsign("XRF*")) )
			continue;
		if ( strlen((*it).GetModules()) != 2 )
			continue;
		if ( peers->FindPeer((*it).GetCallsign(), PROTOCOL_DEXTRA) == nullptr )
		{
			// resolve again peer's IP in case it's a dynamic IP
			(*it).ResolveIp();
			// send connect packet to re-initiate peer link
			EncodeConnectPacket(&buffer, (*it).GetModules());
			Send(buffer, (*it).GetIp(), DEXTRA_PORT);
			std::cout << "Sending connect packet to XRF peer " << (*it).GetCallsign() << " @ " << (*it).GetIp() << " for module " << (*it).GetModules()[1] << " (module " << (*it).GetModules()[0] << ")" << std::endl;
		}
	}

	// done
	g_Reflector.ReleasePeers();
	g_GateKeeper.ReleasePeerList();
}

////////////////////////////////////////////////////////////////////////////////////////
// streams helpers

void CDextraProtocol::OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &Header, const CIp &Ip)
{
	// find the stream
	CPacketStream *stream = GetStream(Header->GetStreamId());
	if ( stream )
	{
		// stream already open
		// skip packet, but tickle the stream
		stream->Tickle();
	}
	else
	{
		// no stream open yet, open a new one
		CCallsign my(Header->GetMyCallsign());
		CCallsign rpt1(Header->GetRpt1Callsign());
		CCallsign rpt2(Header->GetRpt2Callsign());

		// find this client
		std::shared_ptr<CClient>client = g_Reflector.GetClients()->FindClient(Ip, PROTOCOL_DEXTRA);
		if ( client )
		{
			// get client callsign
			rpt1 = client->GetCallsign();
			// apply protocol revision details
			if ( client->GetProtocolRevision() == 2 )
			{
				// update Header RPT2 module letter with
				// the module the client is linked to
				auto m = client->GetReflectorModule();
				Header->SetRpt2Module(m);
				rpt2.SetModule(m);
			}
			// and try to open the stream
			if ( (stream = g_Reflector.OpenStream(Header, client)) != nullptr )
			{
				// keep the handle
				m_Streams.push_back(stream);
			}
		}
		// release
		g_Reflector.ReleaseClients();

		// update last heard
		g_Reflector.GetUsers()->Hearing(my, rpt1, rpt2);
		g_Reflector.ReleaseUsers();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// packet decoding helpers

bool CDextraProtocol::IsValidConnectPacket(const CBuffer &Buffer, CCallsign *callsign, char *reflectormodule, int *revision)
{
	bool valid = false;
	if ((Buffer.size() == 11) && (Buffer.data()[9] != ' '))
	{
		callsign->SetCallsign(Buffer.data(), 8);
		callsign->SetModule(Buffer.data()[8]);
		*reflectormodule = Buffer.data()[9];
		*revision = (Buffer.data()[10] == 11) ? 1 : 0;
		valid = (callsign->IsValid() && IsLetter(*reflectormodule));
		// detect revision
		if ( (Buffer.data()[10] == 11) )
		{
			*revision = 1;
		}
		else if ( callsign->HasSameCallsignWithWildcard(CCallsign("XRF*")) )
		{
			*revision = 2;
		}
		else
		{
			*revision = 0;
		}
	}
	return valid;
}

bool CDextraProtocol::IsValidDisconnectPacket(const CBuffer &Buffer, CCallsign *callsign)
{
	bool valid = false;
	if ((Buffer.size() == 11) && (Buffer.data()[9] == ' '))
	{
		callsign->SetCallsign(Buffer.data(), 8);
		callsign->SetModule(Buffer.data()[8]);
		valid = callsign->IsValid();
	}
	return valid;
}

bool CDextraProtocol::IsValidKeepAlivePacket(const CBuffer &Buffer, CCallsign *callsign)
{
	bool valid = false;
	if (Buffer.size() == 9)
	{
		callsign->SetCallsign(Buffer.data(), 8);
		valid = callsign->IsValid();
	}
	return valid;
}

bool CDextraProtocol::IsValidDvHeaderPacket(const CBuffer &Buffer, std::unique_ptr<CDvHeaderPacket> &header)
{
	if ( 56==Buffer.size() && 0==Buffer.Compare((uint8 *)"DSVT", 4) && 0x10U==Buffer.data()[4] && 0x20U==Buffer.data()[8] )
	{
		// create packet
		header = std::unique_ptr<CDvHeaderPacket>(new CDvHeaderPacket((struct dstar_header *)&(Buffer.data()[15]), *((uint16 *)&(Buffer.data()[12])), 0x80));
		// check validity of packet
		if ( header && header->IsValid() )
			return true;
	}
	return false;
}

bool CDextraProtocol::IsValidDvFramePacket(const CBuffer &Buffer, std::unique_ptr<CDvFramePacket> &dvframe)
{
	if ( 27==Buffer.size() && 0==Buffer.Compare((uint8 *)"DSVT", 4) && 0x20U==Buffer.data()[4] && 0x20U==Buffer.data()[8] && 0U==(Buffer.data()[14] & 0x40U) )
	{
		// create packet
		dvframe = std::unique_ptr<CDvFramePacket>(new CDvFramePacket((struct dstar_dvframe *)&(Buffer.data()[15]), *((uint16 *)&(Buffer.data()[12])), Buffer.data()[14]));
		// check validity of packet
		if ( dvframe && dvframe->IsValid() )
			return true;
	}
	return false;
}

bool CDextraProtocol::IsValidDvLastFramePacket(const CBuffer &Buffer, std::unique_ptr<CDvLastFramePacket> &dvframe)
{
	if ( 27==Buffer.size() && 0==Buffer.Compare((uint8 *)"DSVT", 4) && 0x20U==Buffer.data()[4] && 0x20U==Buffer.data()[8] && (Buffer.data()[14] & 0x40) )
	{
		// create packet
		dvframe = std::unique_ptr<CDvLastFramePacket>(new CDvLastFramePacket((struct dstar_dvframe *)&(Buffer.data()[15]), *((uint16 *)&(Buffer.data()[12])), Buffer.data()[14]));
		// check validity of packet
		if ( dvframe && dvframe->IsValid() )
			return true;
	}
	return false;
}

////////////////////////////////////////////////////////////////////////////////////////
// packet encoding helpers

void CDextraProtocol::EncodeKeepAlivePacket(CBuffer *Buffer)
{
	Buffer->Set(GetReflectorCallsign());
}

void CDextraProtocol::EncodeConnectPacket(CBuffer *Buffer, const char *Modules)
{
	uint8 lm = (uint8)Modules[0];
	uint8 rm = (uint8)Modules[1];
	Buffer->Set((uint8 *)(const char *)GetReflectorCallsign(), CALLSIGN_LEN);
	Buffer->Append(lm);
	Buffer->Append(rm);
	Buffer->Append((uint8)0);
}

void CDextraProtocol::EncodeConnectAckPacket(CBuffer *Buffer, int ProtRev)
{
	// is it for a XRF or repeater
	if ( ProtRev == 2 )
	{
		// XRFxxx
		uint8 rm = (Buffer->data())[8];
		uint8 lm = (Buffer->data())[9];
		Buffer->clear();
		Buffer->Set((uint8 *)(const char *)GetReflectorCallsign(), CALLSIGN_LEN);
		Buffer->Append(lm);
		Buffer->Append(rm);
		Buffer->Append((uint8)0);
	}
	else
	{
		// regular repeater
		uint8 tag[] = { 'A','C','K',0 };
		Buffer->resize(Buffer->size()-1);
		Buffer->Append(tag, sizeof(tag));
	}
}

void CDextraProtocol::EncodeConnectNackPacket(CBuffer *Buffer)
{
	uint8 tag[] = { 'N','A','K',0 };
	Buffer->resize(Buffer->size()-1);
	Buffer->Append(tag, sizeof(tag));
}

void CDextraProtocol::EncodeDisconnectPacket(CBuffer *Buffer, char Module)
{
	uint8 tag[] = { ' ',0 };
	Buffer->Set((uint8 *)(const char *)GetReflectorCallsign(), CALLSIGN_LEN);
	Buffer->Append((uint8)Module);
	Buffer->Append(tag, sizeof(tag));
}

void CDextraProtocol::EncodeDisconnectedPacket(CBuffer *Buffer)
{
	uint8 tag[] = { 'D','I','S','C','O','N','N','E','C','T','E','D' };
	Buffer->Set(tag, sizeof(tag));
}

bool CDextraProtocol::EncodeDvHeaderPacket(const CDvHeaderPacket &Packet, CBuffer *Buffer) const
{
	uint8 tag[]	= { 'D','S','V','T',0x10,0x00,0x00,0x00,0x20,0x00,0x01,0x02 };
	struct dstar_header DstarHeader;

	Packet.ConvertToDstarStruct(&DstarHeader);

	Buffer->Set(tag, sizeof(tag));
	Buffer->Append(Packet.GetStreamId());
	Buffer->Append((uint8)0x80);
	Buffer->Append((uint8 *)&DstarHeader, sizeof(struct dstar_header));

	return true;
}

bool CDextraProtocol::EncodeDvFramePacket(const CDvFramePacket &Packet, CBuffer *Buffer) const
{
	uint8 tag[] = { 'D','S','V','T',0x20,0x00,0x00,0x00,0x20,0x00,0x01,0x02 };

	Buffer->Set(tag, sizeof(tag));
	Buffer->Append(Packet.GetStreamId());
	Buffer->Append((uint8)(Packet.GetPacketId() % 21));
	Buffer->Append((uint8 *)Packet.GetAmbe(), AMBE_SIZE);
	Buffer->Append((uint8 *)Packet.GetDvData(), DVDATA_SIZE);

	return true;

}

bool CDextraProtocol::EncodeDvLastFramePacket(const CDvLastFramePacket &Packet, CBuffer *Buffer) const
{
	uint8 tag1[] = { 'D','S','V','T',0x20,0x00,0x00,0x00,0x20,0x00,0x01,0x02 };
	uint8 tag2[] = { 0x55,0xC8,0x7A,0x00,0x00,0x00,0x00,0x00,0x00,0x25,0x1A,0xC6 };

	Buffer->Set(tag1, sizeof(tag1));
	Buffer->Append(Packet.GetStreamId());
	Buffer->Append((uint8)((Packet.GetPacketId() % 21) | 0x40));
	Buffer->Append(tag2, sizeof(tag2));

	return true;
}
