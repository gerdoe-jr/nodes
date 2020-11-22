/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <base/math.h>
#include <base/system.h>

#include <engine/external/json-parser/json.h>

#include <engine/shared/config.h>
#include <engine/shared/memheap.h>
#include <engine/shared/network.h>
#include <engine/shared/packer.h>
#include <engine/shared/jsonwriter.h>

#include <engine/config.h>
#include <engine/console.h>
#include <engine/engine.h>
#include <engine/contacts.h>
#include <engine/storage.h>
#include <engine/http.h>

#include "serverbrowser.h"


static const char *s_pFilename = "serverlist.json";

inline int AddrHash(const NETADDR *pAddr)
{
	if(pAddr->type==NETTYPE_IPV4)
		return (pAddr->ip[0]+pAddr->ip[1]+pAddr->ip[2]+pAddr->ip[3])&0xFF;
	else
		return (pAddr->ip[0]+pAddr->ip[1]+pAddr->ip[2]+pAddr->ip[3]+pAddr->ip[4]+pAddr->ip[5]+pAddr->ip[6]+pAddr->ip[7]+
			pAddr->ip[8]+pAddr->ip[9]+pAddr->ip[10]+pAddr->ip[11]+pAddr->ip[12]+pAddr->ip[13]+pAddr->ip[14]+pAddr->ip[15])&0xFF;
}

inline int GetNewToken()
{
	return random_int();
}

//
void CServerBrowser::CServerlist::Clear()
{
	m_ServerlistHeap.Reset();
	m_NumServers = 0;
	m_NumPlayers = 0;
	m_NumClients = 0;
	mem_zero(m_aServerlistIp, sizeof(m_aServerlistIp));
}

//
CServerBrowser::CServerBrowser()
{
	//
	for(int i = 0; i < NUM_TYPES; ++i)
	{
		m_aServerlist[i].Clear();
		m_aServerlist[i].m_NumServerCapacity = 0;
		m_aServerlist[i].m_ppServerlist = 0;
	}

	m_pFirstReqServer = 0; // request list
	m_pLastReqServer = 0;
	m_NumRequests = 0;

	m_NeedRefresh = 0;
	m_RefreshFlags = 0;

	// the token is to keep server refresh separated from each other
	m_CurrentLanToken = 1;

	m_ActServerlistType = 0;
	m_BroadcastTime = 0;
	m_MasterRefreshTime = 0;

	m_RefreshingServerlist = false;
}

void CServerBrowser::Init(class CNetClient *pNetClient, const char *pNetVersion)
{
	IConfigManager *pConfigManager = Kernel()->RequestInterface<IConfigManager>();
	m_pConfig = pConfigManager->Values();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_pHttp = Kernel()->RequestInterface<IHttp>();
	m_pNetClient = pNetClient;

	m_ServerBrowserFavorites.Init(pNetClient, m_pConsole, Kernel()->RequestInterface<IEngine>(), pConfigManager);
	m_ServerBrowserFilter.Init(Config(), Kernel()->RequestInterface<IFriends>(), pNetVersion);
}

void CServerBrowser::Set(const NETADDR &Addr, int SetType, int Token, const CServerInfo *pInfo)
{
	CServerEntry *pEntry = 0;
	switch(SetType)
	{
	case SET_MASTER_ADD:
		{
			if(!(m_RefreshFlags&IServerBrowser::REFRESHFLAG_INTERNET))
				return;

			m_MasterRefreshTime = 0;

			if(!Find(IServerBrowser::TYPE_INTERNET, Addr))
			{
				pEntry = Add(IServerBrowser::TYPE_INTERNET, Addr);
				QueueRequest(pEntry);
			}
		}
		break;
	case SET_FAV_ADD:
		{
			if(!(m_RefreshFlags&IServerBrowser::REFRESHFLAG_INTERNET))
				return;

			if(!Find(IServerBrowser::TYPE_INTERNET, Addr))
			{
				pEntry = Add(IServerBrowser::TYPE_INTERNET, Addr);
				QueueRequest(pEntry);
			}
		}
		break;
	case SET_TOKEN:
		{
			int Type;

			// internet entry
			if(m_RefreshFlags&IServerBrowser::REFRESHFLAG_INTERNET)
			{
				Type = IServerBrowser::TYPE_INTERNET;
				pEntry = Find(Type, Addr);
				if(pEntry && (pEntry->m_InfoState != CServerEntry::STATE_PENDING || Token != pEntry->m_CurrentToken))
					pEntry = 0;
			}

			// lan entry
			if(!pEntry && (m_RefreshFlags&IServerBrowser::REFRESHFLAG_LAN) && m_BroadcastTime+time_freq() >= time_get())
			{
				Type = IServerBrowser::TYPE_LAN;
				pEntry = Add(Type, Addr);
			}

			// set info
			if(pEntry)
			{
				SetInfo(Type, pEntry, *pInfo);
				if(Type == IServerBrowser::TYPE_LAN)
					pEntry->m_Info.m_Latency = min(static_cast<int>((time_get()-m_BroadcastTime)*1000/time_freq()), 999);
				else
					pEntry->m_Info.m_Latency = min(static_cast<int>((time_get()-pEntry->m_RequestTime)*1000/time_freq()), 999);
				RemoveRequest(pEntry);
			}
		}
		break;
	case SET_HTTP:
		{
			// internet entry
			int Type = IServerBrowser::TYPE_INTERNET;
			pEntry = Find(Type, Addr);
			if(!pEntry)
				pEntry = Add(Type, Addr);
			else if (pEntry && (pEntry->m_InfoState != CServerEntry::STATE_PENDING))
				pEntry = 0;

			m_MasterRefreshTime = 0;

			// set info
			if (pEntry)
			{
				SetInfo(Type, pEntry, *pInfo);
				pEntry->m_Info.m_Latency = 999;
				// RemoveRequest(pEntry); // needed?
			}
		}
	}

	if(pEntry)
		m_ServerBrowserFilter.Sort(m_aServerlist[m_ActServerlistType].m_ppServerlist, m_aServerlist[m_ActServerlistType].m_NumServers, CServerBrowserFilter::RESORT_FLAG_FORCE);
}

void CServerBrowser::Update(bool ForceResort)
{
	int64 Timeout = time_freq();
	int64 Now = time_get();
	int Count;
	CServerEntry *pEntry, *pNext;

	// do server list requests
	if(m_NeedRefresh && !m_RefreshingServerlist)
	{
		m_NeedRefresh = 0;

		RequestServerlist();
		m_MasterRefreshTime = Now;

		if(Config()->m_Debug)
			m_pConsole->Print(IConsole::OUTPUT_LEVEL_DEBUG, "client_srvbrowse", "requesting server list");
	}

	// load server list backup from file in case the masters don't response
	if(m_MasterRefreshTime && m_MasterRefreshTime+2*Timeout < Now)
	{
		LoadServerlist();
		m_MasterRefreshTime = 0;

		if(Config()->m_Debug)
			m_pConsole->Print(IConsole::OUTPUT_LEVEL_DEBUG, "client_srvbrowse", "using backup server list");
	}

	// do timeouts
	pEntry = m_pFirstReqServer;
	while(1)
	{
		if(!pEntry) // no more entries
			break;

		pNext = pEntry->m_pNextReq;

		if(pEntry->m_RequestTime && pEntry->m_RequestTime+Timeout < Now)
		{
			// timeout
			RemoveRequest(pEntry);
		}

		pEntry = pNext;
	}

	// do timeouts
	pEntry = m_pFirstReqServer;
	Count = 0;
	while(1)
	{
		if(!pEntry) // no more entries
			break;

		// no more then 10 concurrent requests
		if(Count == Config()->m_BrMaxRequests)
			break;

		if(pEntry->m_RequestTime == 0)
			RequestImpl(pEntry->m_Addr, pEntry);

		Count++;
		pEntry = pEntry->m_pNextReq;
	}

	// update favorite
	const NETADDR *pFavAddr = m_ServerBrowserFavorites.UpdateFavorites();
	if(pFavAddr)
	{
		for(int i = 0; i < NUM_TYPES; ++i)
		{
			CServerEntry *pEntry = Find(i, *pFavAddr);
			if(pEntry)
				pEntry->m_Info.m_Favorite = true;

			if(i == m_ActServerlistType)
				ForceResort = true;
		}
	}

	m_ServerBrowserFilter.Sort(m_aServerlist[m_ActServerlistType].m_ppServerlist, m_aServerlist[m_ActServerlistType].m_NumServers, ForceResort ? CServerBrowserFilter::RESORT_FLAG_FORCE : 0);
}

// interface functions
void CServerBrowser::SetType(int Type)
{
	if(Type < 0 || Type >= NUM_TYPES || Type == m_ActServerlistType)
		return;

	m_ActServerlistType = Type;
	m_ServerBrowserFilter.Sort(m_aServerlist[m_ActServerlistType].m_ppServerlist, m_aServerlist[m_ActServerlistType].m_NumServers, CServerBrowserFilter::RESORT_FLAG_FORCE);
}

void CServerBrowser::Refresh(int RefreshFlags)
{
	m_RefreshFlags |= RefreshFlags;

	if(RefreshFlags&IServerBrowser::REFRESHFLAG_LAN)
	{
		// clear out everything
		m_aServerlist[IServerBrowser::TYPE_LAN].Clear();
		if(m_ActServerlistType == IServerBrowser::TYPE_LAN)
			m_ServerBrowserFilter.Clear();

		// next token
		m_CurrentLanToken = GetNewToken();

		CPacker Packer;
		Packer.Reset();
		Packer.AddRaw(SERVERBROWSE_GETINFO, sizeof(SERVERBROWSE_GETINFO));
		Packer.AddInt(m_CurrentLanToken);

		/* do the broadcast version */
		CNetChunk Packet;
		mem_zero(&Packet, sizeof(Packet));
		Packet.m_Address.type = m_pNetClient->NetType()|NETTYPE_LINK_BROADCAST;
		Packet.m_ClientID = -1;
		Packet.m_Flags = NETSENDFLAG_CONNLESS;
		Packet.m_DataSize = Packer.Size();
		Packet.m_pData = Packer.Data();
		m_BroadcastTime = time_get();

		for(int i = 8303; i <= 8310; i++)
		{
			Packet.m_Address.port = i;
			m_pNetClient->Send(&Packet);
		}

		if(Config()->m_Debug)
			m_pConsole->Print(IConsole::OUTPUT_LEVEL_DEBUG, "client_srvbrowse", "broadcasting for servers");
	}

	if(RefreshFlags&IServerBrowser::REFRESHFLAG_INTERNET)
	{
		// clear out everything
		for(CServerEntry *pEntry = m_pFirstReqServer; pEntry; pEntry = pEntry->m_pNextReq)
		{
			m_pNetClient->PurgeStoredPacket(pEntry->m_TrackID);
		}
		m_aServerlist[IServerBrowser::TYPE_INTERNET].Clear();
		if(m_ActServerlistType == IServerBrowser::TYPE_INTERNET)
			m_ServerBrowserFilter.Clear();
		m_pFirstReqServer = 0;
		m_pLastReqServer = 0;
		m_NumRequests = 0;

		m_NeedRefresh = 1;
		for(int i = 0; i < m_ServerBrowserFavorites.m_NumFavoriteServers; i++)
			if(m_ServerBrowserFavorites.m_aFavoriteServers[i].m_State >= CServerBrowserFavorites::FAVSTATE_ADDR)
				Set(m_ServerBrowserFavorites.m_aFavoriteServers[i].m_Addr, SET_FAV_ADD, -1, 0);
	}
}

int CServerBrowser::LoadingProgression() const
{
	if(m_aServerlist[m_ActServerlistType].m_NumServers == 0)
		return 0;

	int Servers = m_aServerlist[m_ActServerlistType].m_NumServers;
	int Loaded = m_aServerlist[m_ActServerlistType].m_NumServers-m_NumRequests;
	return 100.0f * Loaded/Servers;
}

void CServerBrowser::AddFavorite(const CServerInfo *pInfo)
{
	if(m_ServerBrowserFavorites.AddFavoriteEx(pInfo->m_aHostname, &pInfo->m_NetAddr, true))
	{
		for(int i = 0; i < NUM_TYPES; ++i)
		{
			CServerEntry *pEntry = Find(i, pInfo->m_NetAddr);
			if(pEntry)
			{
				pEntry->m_Info.m_Favorite = true;

				// refresh servers in all filters where favorites are filtered
				if(i == m_ActServerlistType)
					m_ServerBrowserFilter.Sort(m_aServerlist[m_ActServerlistType].m_ppServerlist, m_aServerlist[m_ActServerlistType].m_NumServers, CServerBrowserFilter::RESORT_FLAG_FAV);
			}
		}
	}
}

void CServerBrowser::RemoveFavorite(const CServerInfo *pInfo)
{
	if(m_ServerBrowserFavorites.RemoveFavoriteEx(pInfo->m_aHostname, &pInfo->m_NetAddr))
	{
		for(int i = 0; i < NUM_TYPES; ++i)
		{
			CServerEntry *pEntry = Find(i, pInfo->m_NetAddr);
			if(pEntry)
			{
				pEntry->m_Info.m_Favorite = false;

				// refresh servers in all filters where favorites are filtered
				if(i == m_ActServerlistType)
					m_ServerBrowserFilter.Sort(m_aServerlist[m_ActServerlistType].m_ppServerlist, m_aServerlist[m_ActServerlistType].m_NumServers, CServerBrowserFilter::RESORT_FLAG_FAV);
			}
		}
	}
}

void CServerBrowser::UpdateFavoriteState(CServerInfo *pInfo)
{
	pInfo->m_Favorite = m_ServerBrowserFavorites.FindFavoriteByAddr(pInfo->m_NetAddr, 0) != 0
		|| m_ServerBrowserFavorites.FindFavoriteByHostname(pInfo->m_aHostname, 0) != 0;
}

void CServerBrowser::SetFavoritePassword(const char *pAddress, const char *pPassword)
{
	if(m_ServerBrowserFavorites.AddFavoriteEx(pAddress, 0, false, pPassword))
	{
		NETADDR Addr = {0};
		if(net_addr_from_str(&Addr, pAddress))
			return;
		for(int i = 0; i < NUM_TYPES; ++i)
		{
			CServerEntry *pEntry = Find(i, Addr);
			if(pEntry)
			{
				pEntry->m_Info.m_Favorite = true;

				// refresh servers in all filters where favorites are filtered
				if(i == m_ActServerlistType)
					m_ServerBrowserFilter.Sort(m_aServerlist[m_ActServerlistType].m_ppServerlist, m_aServerlist[m_ActServerlistType].m_NumServers, CServerBrowserFilter::RESORT_FLAG_FAV);
			}
		}
	}
}

const char *CServerBrowser::GetFavoritePassword(const char *pAddress)
{
	NETADDR Addr = {0};
	if(net_addr_from_str(&Addr, pAddress))
		return 0;
	CServerBrowserFavorites::CFavoriteServer *pFavorite = m_ServerBrowserFavorites.FindFavoriteByAddr(Addr, 0);
	if(!pFavorite)
		return 0;
	if(!pFavorite->m_aPassword[0])
		return 0;
	return pFavorite->m_aPassword;
}

// manipulate entries
CServerEntry *CServerBrowser::Add(int ServerlistType, const NETADDR &Addr)
{
	// create new pEntry
	CServerEntry *pEntry = (CServerEntry *)m_aServerlist[ServerlistType].m_ServerlistHeap.Allocate(sizeof(CServerEntry));
	mem_zero(pEntry, sizeof(CServerEntry));

	// set the info
	pEntry->m_Addr = Addr;
	pEntry->m_InfoState = CServerEntry::STATE_INVALID;
	pEntry->m_CurrentToken = GetNewToken();
	pEntry->m_Info.m_NetAddr = Addr;

	pEntry->m_Info.m_Latency = 999;
	net_addr_str(&Addr, pEntry->m_Info.m_aAddress, sizeof(pEntry->m_Info.m_aAddress), true);
	str_copy(pEntry->m_Info.m_aName, pEntry->m_Info.m_aAddress, sizeof(pEntry->m_Info.m_aName));
	str_copy(pEntry->m_Info.m_aHostname, pEntry->m_Info.m_aAddress, sizeof(pEntry->m_Info.m_aHostname));

	UpdateFavoriteState(&pEntry->m_Info);

	// add to the hash list
	int Hash = AddrHash(&Addr);
	pEntry->m_pNextIp = m_aServerlist[ServerlistType].m_aServerlistIp[Hash];
	m_aServerlist[ServerlistType].m_aServerlistIp[Hash] = pEntry;

	if(m_aServerlist[ServerlistType].m_NumServers == m_aServerlist[ServerlistType].m_NumServerCapacity)
	{
		if(m_aServerlist[ServerlistType].m_NumServerCapacity == 0)
		{
			// alloc start size
			m_aServerlist[ServerlistType].m_NumServerCapacity = 1000;
			m_aServerlist[ServerlistType].m_ppServerlist = (CServerEntry **)mem_alloc(m_aServerlist[ServerlistType].m_NumServerCapacity*sizeof(CServerEntry*), 1);
		}
		else
		{
			// increase size
			m_aServerlist[ServerlistType].m_NumServerCapacity += 100;
			CServerEntry **ppNewlist = (CServerEntry **)mem_alloc(m_aServerlist[ServerlistType].m_NumServerCapacity*sizeof(CServerEntry*), 1);
			mem_copy(ppNewlist, m_aServerlist[ServerlistType].m_ppServerlist, m_aServerlist[ServerlistType].m_NumServers*sizeof(CServerEntry*));
			mem_free(m_aServerlist[ServerlistType].m_ppServerlist);
			m_aServerlist[ServerlistType].m_ppServerlist = ppNewlist;
		}
	}

	// add to list
	m_aServerlist[ServerlistType].m_ppServerlist[m_aServerlist[ServerlistType].m_NumServers] = pEntry;
	pEntry->m_Info.m_ServerIndex = m_aServerlist[ServerlistType].m_NumServers;
	m_aServerlist[ServerlistType].m_NumServers++;

	return pEntry;
}

CServerEntry *CServerBrowser::Find(int ServerlistType, const NETADDR &Addr)
{
	for(CServerEntry *pEntry = m_aServerlist[ServerlistType].m_aServerlistIp[AddrHash(&Addr)]; pEntry; pEntry = pEntry->m_pNextIp)
	{
		if(net_addr_comp(&pEntry->m_Addr, &Addr) == 0)
			return pEntry;
	}
	return (CServerEntry*)0;
}

void CServerBrowser::QueueRequest(CServerEntry *pEntry)
{
	// add it to the list of servers that we should request info from
	pEntry->m_pPrevReq = m_pLastReqServer;
	if(m_pLastReqServer)
		m_pLastReqServer->m_pNextReq = pEntry;
	else
		m_pFirstReqServer = pEntry;
	m_pLastReqServer = pEntry;

	m_NumRequests++;
}

void CServerBrowser::RemoveRequest(CServerEntry *pEntry)
{
	if(pEntry->m_pPrevReq || pEntry->m_pNextReq || m_pFirstReqServer == pEntry)
	{
		if(pEntry->m_pPrevReq)
			pEntry->m_pPrevReq->m_pNextReq = pEntry->m_pNextReq;
		else
			m_pFirstReqServer = pEntry->m_pNextReq;

		if(pEntry->m_pNextReq)
			pEntry->m_pNextReq->m_pPrevReq = pEntry->m_pPrevReq;
		else
			m_pLastReqServer = pEntry->m_pPrevReq;

		pEntry->m_pPrevReq = 0;
		pEntry->m_pNextReq = 0;
		m_NumRequests--;
	}
}

void CServerBrowser::CBFTrackPacket(int TrackID, void *pCallbackUser)
{
	if(!pCallbackUser)
		return;

	CServerBrowser *pSelf = (CServerBrowser *)pCallbackUser;
	CServerEntry *pEntry = pSelf->m_pFirstReqServer;
	while(1)
	{
		if(!pEntry)	// no more entries
			break;

		if(pEntry->m_TrackID == TrackID)	// got it -> update
		{
			pEntry->m_RequestTime = time_get();
			break;
		}

		pEntry = pEntry->m_pNextReq;
	}
}

void CServerBrowser::RequestImpl(const NETADDR &Addr, CServerEntry *pEntry)
{
	if(Config()->m_Debug)
	{
		char aAddrStr[NETADDR_MAXSTRSIZE];
		net_addr_str(&Addr, aAddrStr, sizeof(aAddrStr), true);
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf),"requesting server info from %s", aAddrStr);
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_DEBUG, "client_srvbrowse", aBuf);
	}

	CPacker Packer;
	Packer.Reset();
	Packer.AddRaw(SERVERBROWSE_GETINFO, sizeof(SERVERBROWSE_GETINFO));
	Packer.AddInt(pEntry ? pEntry->m_CurrentToken : m_CurrentLanToken);

	CNetChunk Packet;
	Packet.m_ClientID = -1;
	Packet.m_Address = Addr;
	Packet.m_Flags = NETSENDFLAG_CONNLESS;
	Packet.m_DataSize = Packer.Size();
	Packet.m_pData = Packer.Data();
	CSendCBData Data;
	Data.m_pfnCallback = CBFTrackPacket;
	Data.m_pCallbackUser = this;
	m_pNetClient->Send(&Packet, NET_TOKEN_NONE, &Data);

	if(pEntry)
	{
		pEntry->m_TrackID = Data.m_TrackID;
		pEntry->m_RequestTime = time_get();
		pEntry->m_InfoState = CServerEntry::STATE_PENDING;
	}
}

void CServerBrowser::SetInfo(int ServerlistType, CServerEntry *pEntry, const CServerInfo &Info)
{
	bool Fav = pEntry->m_Info.m_Favorite;
	pEntry->m_Info = Info;
	pEntry->m_Info.m_Flags &= FLAG_PASSWORD|FLAG_TIMESCORE;
	if(str_comp(pEntry->m_Info.m_aGameType, "DM") == 0 || str_comp(pEntry->m_Info.m_aGameType, "TDM") == 0 || str_comp(pEntry->m_Info.m_aGameType, "CTF") == 0 ||
		str_comp(pEntry->m_Info.m_aGameType, "LTS") == 0 ||	str_comp(pEntry->m_Info.m_aGameType, "LMS") == 0)
		pEntry->m_Info.m_Flags |= FLAG_PURE;

	if(str_comp(pEntry->m_Info.m_aMap, "dm1") == 0 || str_comp(pEntry->m_Info.m_aMap, "dm2") == 0 || str_comp(pEntry->m_Info.m_aMap, "dm3") == 0 ||
		str_comp(pEntry->m_Info.m_aMap, "dm6") == 0 || str_comp(pEntry->m_Info.m_aMap, "dm7") == 0 || str_comp(pEntry->m_Info.m_aMap, "dm8") == 0 ||
		str_comp(pEntry->m_Info.m_aMap, "dm9") == 0 ||
		str_comp(pEntry->m_Info.m_aMap, "ctf1") == 0 || str_comp(pEntry->m_Info.m_aMap, "ctf2") == 0 || str_comp(pEntry->m_Info.m_aMap, "ctf3") == 0 ||
		str_comp(pEntry->m_Info.m_aMap, "ctf4") == 0 || str_comp(pEntry->m_Info.m_aMap, "ctf5") == 0 || str_comp(pEntry->m_Info.m_aMap, "ctf6") == 0 ||
		str_comp(pEntry->m_Info.m_aMap, "ctf7") == 0 || str_comp(pEntry->m_Info.m_aMap, "ctf8") == 0 ||
		str_comp(pEntry->m_Info.m_aMap, "lms1") == 0)
		pEntry->m_Info.m_Flags |= FLAG_PUREMAP;
	pEntry->m_Info.m_Favorite = Fav;
	pEntry->m_Info.m_NetAddr = pEntry->m_Addr;

	m_aServerlist[ServerlistType].m_NumPlayers += pEntry->m_Info.m_NumPlayers;
	m_aServerlist[ServerlistType].m_NumClients += pEntry->m_Info.m_NumClients;

	pEntry->m_InfoState = CServerEntry::STATE_READY;
}

void CServerBrowser::LoadServerlist()
{
	// read file data into buffer
	IOHANDLE File = Storage()->OpenFile(s_pFilename, IOFLAG_READ, IStorage::TYPE_ALL);
	if(!File)
		return;
	int FileSize = (int)io_length(File);
	char *pFileData = (char *)mem_alloc(FileSize, 1);
	io_read(File, pFileData, FileSize);
	io_close(File);

	// parse json data
	json_settings JsonSettings;
	mem_zero(&JsonSettings, sizeof(JsonSettings));
	char aError[256];
	json_value *pJsonData = json_parse_ex(&JsonSettings, pFileData, FileSize, aError);
	mem_free(pFileData);

	if(pJsonData == 0)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, s_pFilename, aError);
		return;
	}

	// extract server list
	const json_value &rEntry = (*pJsonData)["serverlist"];
	for(unsigned i = 0; i < rEntry.u.array.length; ++i)
	{
		if(rEntry[i].type == json_string)
		{
			NETADDR Addr = { 0 };
			if(!net_addr_from_str(&Addr, rEntry[i]))
				Set(Addr, SET_MASTER_ADD, -1, 0);
		}
	}

	// clean up
	json_value_free(pJsonData);
}

void CServerBrowser::SaveServerlist()
{
	IOHANDLE File = Storage()->OpenFile(s_pFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
		return;

	CJsonWriter Writer(File);
	Writer.BeginObject(); // root
	Writer.WriteAttribute("serverlist");
	Writer.BeginArray();
	for(int i = 0; i < m_aServerlist[IServerBrowser::TYPE_INTERNET].m_NumServers; ++i)
		Writer.WriteStrValue(m_aServerlist[IServerBrowser::TYPE_INTERNET].m_ppServerlist[i]->m_Info.m_aAddress);
	Writer.EndArray();
	Writer.EndObject();
}

void CServerBrowser::ServerListCallback(void* pUser, int ResponseCode, std::string Response)
{
	CServerBrowser* pSelf = (CServerBrowser*)pUser;

	pSelf->m_RefreshingServerlist = false;

	if (ResponseCode == 200)
	{
		// parse json data
		json_settings JsonSettings;
		mem_zero(&JsonSettings, sizeof(JsonSettings));

		char aError[256];
		json_value* pJsonData = json_parse_ex(&JsonSettings, Response.c_str(), Response.size(), aError);
		if (pJsonData == 0 || (*pJsonData).type != json_object || str_comp((*pJsonData).u.object.values[0].name, "server_list"))
		{
			dbg_msg("master", "failed to parse server list: %s", aError);
			return;
		}

		json_value* pServerList = (*pJsonData).u.object.values[0].value;
		if (pServerList->type != json_array)
			return;

		for (unsigned int i = 0; i < pServerList->u.array.length; i++)
		{
			int NumPlayers = 0;
			int NumClients = 0;
			CServerInfo Info = { 0 };
			json_value* pEntry = pServerList->u.array.values[i];

			for (unsigned int j = 0; j < pEntry->u.object.length; j++)
			{
				json_value* pItem = pEntry->u.object.values[j].value;

				if (!str_comp_nocase(pEntry->u.object.values[j].name, "ipv4"))
					str_copy(Info.m_aAddress, pItem->u.string.ptr, sizeof(Info.m_aAddress));
				else if (!str_comp_nocase(pEntry->u.object.values[j].name, "port"))
				{
					char aPort[8];
					str_format(aPort, sizeof(aPort), ":%d", (int)pItem->u.integer);
					str_append(Info.m_aAddress, aPort, sizeof(Info.m_aAddress));
				}
				else if (!str_comp_nocase(pEntry->u.object.values[j].name, "version"))
					str_copy(Info.m_aVersion, pItem->u.string.ptr, sizeof(Info.m_aVersion));
				else if (!str_comp_nocase(pEntry->u.object.values[j].name, "name"))
				{
					str_copy(Info.m_aName, pItem->u.string.ptr, sizeof(Info.m_aName));
					str_clean_whitespaces(Info.m_aName);
					// str_append(Info.m_aName, " (http)", sizeof(Info.m_aName)); // debug
				}
				else if (!str_comp_nocase(pEntry->u.object.values[j].name, "hostname"))
				{
					str_copy(Info.m_aHostname, pItem->u.string.ptr, sizeof(Info.m_aHostname));
					if (Info.m_aHostname[0] == 0)
						str_copy(Info.m_aHostname, Info.m_aAddress, sizeof(Info.m_aHostname));
				}
				else if (!str_comp_nocase(pEntry->u.object.values[j].name, "gamemode"))
					str_copy(Info.m_aGameType, pItem->u.string.ptr, sizeof(Info.m_aGameType));
				else if (!str_comp_nocase(pEntry->u.object.values[j].name, "map"))
					str_copy(Info.m_aMap, pItem->u.string.ptr, sizeof(Info.m_aMap));
				else if (!str_comp_nocase(pEntry->u.object.values[j].name, "flags"))
				{
					int Flags = (int)pItem->u.integer;
					Info.m_Flags = 0;
					if (Flags & SERVERINFO_FLAG_PASSWORD)
						Info.m_Flags |= IServerBrowser::FLAG_PASSWORD;
					if (Flags & SERVERINFO_FLAG_TIMESCORE)
						Info.m_Flags |= IServerBrowser::FLAG_TIMESCORE;
				}
				else if (!str_comp_nocase(pEntry->u.object.values[j].name, "skill_level"))
					Info.m_ServerLevel = clamp<int>((int)pItem->u.integer, SERVERINFO_LEVEL_MIN, SERVERINFO_LEVEL_MAX);
				else if (!str_comp_nocase(pEntry->u.object.values[j].name, "players_online"))
					Info.m_NumPlayers = (int)pItem->u.integer;
				else if (!str_comp_nocase(pEntry->u.object.values[j].name, "players_max"))
					Info.m_MaxPlayers = (int)pItem->u.integer;
				else if (!str_comp_nocase(pEntry->u.object.values[j].name, "clients_online"))
					Info.m_NumClients = (int)pItem->u.integer;
				else if (!str_comp_nocase(pEntry->u.object.values[j].name, "clients_max"))
					Info.m_MaxClients = (int)pItem->u.integer;
				else if (!str_comp_nocase(pEntry->u.object.values[j].name, "player_list"))
				{
					for (unsigned int c = 0; c < pItem->u.array.length; c++)
					{
						json_value* pClientProp = pItem->u.array.values[c];

						for (unsigned int p = 0; p < pClientProp->u.object.length; p++)
						{
							if (!str_comp_nocase(pClientProp->u.object.values[p].name, "name"))
								str_copy(Info.m_aClients[i].m_aName, pClientProp->u.object.values[p].value->u.string.ptr, sizeof(Info.m_aClients[i].m_aName));
							else if (!str_comp_nocase(pClientProp->u.object.values[p].name, "clan"))
								str_copy(Info.m_aClients[i].m_aClan, pClientProp->u.object.values[p].value->u.string.ptr, sizeof(Info.m_aClients[i].m_aClan));
							else if (!str_comp_nocase(pClientProp->u.object.values[p].name, "country"))
								Info.m_aClients[i].m_Country = (int)pClientProp->u.object.values[p].value->u.integer;
							else if (!str_comp_nocase(pClientProp->u.object.values[p].name, "score"))
								Info.m_aClients[i].m_Score = (int)pClientProp->u.object.values[p].value->u.integer;
							else if (!str_comp_nocase(pClientProp->u.object.values[p].name, "type"))
								Info.m_aClients[i].m_PlayerType = (int)pClientProp->u.object.values[p].value->u.integer;
						}
					}
				}
			}

			Info.m_NumBotPlayers = 0;
			Info.m_NumBotSpectators = 0;

			// don't add invalid info to the server browser list
			if (Info.m_NumClients < 0 || Info.m_NumClients > Info.m_MaxClients || Info.m_MaxClients < 0 || Info.m_MaxClients > MAX_CLIENTS || Info.m_MaxPlayers < Info.m_NumPlayers ||
				Info.m_NumPlayers < 0 || Info.m_NumPlayers > Info.m_NumClients || Info.m_MaxPlayers < 0 || Info.m_MaxPlayers > Info.m_MaxClients)
				continue;

			// drop standard gametype with more than MAX_PLAYERS
			if (Info.m_MaxPlayers > MAX_PLAYERS && (str_comp(Info.m_aGameType, "DM") == 0 || str_comp(Info.m_aGameType, "TDM") == 0 || str_comp(Info.m_aGameType, "CTF") == 0 ||
				str_comp(Info.m_aGameType, "LTS") == 0 || str_comp(Info.m_aGameType, "LMS") == 0))
				continue;

			Info.m_NumPlayers = NumPlayers;
			Info.m_NumClients = NumClients;

			NETADDR Addr;
			if (net_addr_from_str(&Addr, Info.m_aAddress) == 0)
			{
				// SortClients(&Info);
				pSelf->Set(Addr, CServerBrowser::SET_HTTP, -1, &Info);
			}
		}

		// clean up
		json_value_free(pJsonData);
	}
	else
		dbg_msg("master", "server list request failed (%d): %s", ResponseCode, Response.c_str());
}

void CServerBrowser::RequestServerlist()
{
	CHttpRequest* pRequest = Http()->PrepareRequest("https://master.teeworlds.network/?modification=nodes");
	pRequest->m_pUser = this;
	pRequest->m_pCallback = ServerListCallback;
	Http()->ExecuteRequest(pRequest);

	m_RefreshingServerlist = true;
}