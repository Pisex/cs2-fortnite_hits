#include <stdio.h>
#include "fortnite_hits.h"
#include <fstream>
#include "entitykeyvalues.h"
#include "schemasystem/schemasystem.h"

Fortnite_Hits g_Fortnite_Hits;
PLUGIN_EXPOSE(Fortnite_Hits, g_Fortnite_Hits);

IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars *gpGlobals = nullptr;

IUtilsApi* g_pUtils;
IPlayersApi* g_pPlayers;

bool g_bIsFired[64],
	g_bIsCrit[64][64],
	g_bIsFirstTime[64],
	g_bState[64],
	g_bHasAccess[64];
int g_iTotalSGDamage[64][64];
Vector g_fPlayerPosLate[64];

bool g_bEnable[64];
float g_fDistance;
bool g_bFreeAccess;

FortniteHitsApi* g_pFHApi = nullptr;
IFortniteHitsApi* g_pFHCore = nullptr;

KeyValues* g_hKVData;

std::map<std::string, std::string> g_vecPhrases;

std::vector<CHandle<CParticleSystem>> g_vPRTDamage[64];

SH_DECL_HOOK7_void(ISource2GameEntities, CheckTransmit, SH_NOATTRIB, 0, CCheckTransmitInfo **, int, CBitVec<16384> &, const Entity2Networkable_t **, const uint16 *, int, bool);

CGameEntitySystem* GameEntitySystem()
{
	return g_pUtils->GetCGameEntitySystem();
}

void StartupServer()
{
	g_pGameEntitySystem = GameEntitySystem();
	g_pEntitySystem = g_pUtils->GetCEntitySystem();
	gpGlobals = g_pUtils->GetCGlobalVars();
}

bool Fortnite_Hits::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameEntities, ISource2GameEntities, SOURCE2GAMEENTITIES_INTERFACE_VERSION);
	SH_ADD_HOOK(ISource2GameEntities, CheckTransmit, g_pSource2GameEntities, SH_MEMBER(this, &Fortnite_Hits::Hook_CheckTransmit), true);

	g_SMAPI->AddListener( this, this );

	g_pFHApi = new FortniteHitsApi();
	g_pFHCore = g_pFHApi;
	return true;
}

void* Fortnite_Hits::OnMetamodQuery(const char* iface, int* ret)
{
	if (!strcmp(iface, FH_INTERFACE))
	{
		*ret = META_IFACE_OK;
		return g_pFHCore;
	}

	*ret = META_IFACE_FAILED;
	return nullptr;
}

void Fortnite_Hits::Hook_CheckTransmit(CCheckTransmitInfo **ppInfoList, int infoCount, CBitVec<16384> &unionTransmitEdicts, const Entity2Networkable_t **pNetworkables, const uint16 *pEntityIndicies, int nEntities, bool bEnablePVSBits)
{
	if (!g_pEntitySystem)
		return;

	for (int i = 0; i < infoCount; i++)
	{
		auto &pInfo = ppInfoList[i];
		int iPlayerSlot = (int)*((uint8 *)pInfo + 584);
		CCSPlayerController* pSelfController = CCSPlayerController::FromSlot(iPlayerSlot);
		if (!pSelfController || !pSelfController->IsConnected())
			continue;
		
		for (int j = 0; j < 64; j++)
		{
			CCSPlayerController* pController = CCSPlayerController::FromSlot(j);

			if (!pController || j == iPlayerSlot)
				continue;

			bool bObserver = pSelfController->GetPawnState() == STATE_OBSERVER_MODE && pSelfController->GetObserverTarget() == pController->GetPawn();
			for(auto &pEnt : g_vPRTDamage[j])
			{
				if(pEnt && !bObserver)
					pInfo->m_pTransmitEntity->Clear(pEnt->entindex());
			}
		}
	}
}

bool Fortnite_Hits::Unload(char *error, size_t maxlen)
{
	ConVar_Unregister();
	SH_REMOVE_HOOK(ISource2GameEntities, CheckTransmit, g_pSource2GameEntities, SH_MEMBER(this, &Fortnite_Hits::Hook_CheckTransmit), true);
	
	return true;
}

void FortniteHitsApi::GiveClientAccess(int iSlot)
{
	g_bHasAccess[iSlot] = true;
}

void FortniteHitsApi::TakeClientAccess(int iSlot)
{
	g_bHasAccess[iSlot] = false;
}

bool IsValidClient(int client, bool botcheck = true)
{
	return (0 <= client && client < 64 && g_pPlayers->IsConnected(client) && g_pPlayers->IsInGame(client) && (botcheck ? !g_pPlayers->IsFakeClient(client) : true)); 
}

int RoundToCeil(float value) {
    return static_cast<int>(ceil(value));
}

float GetRandomFloat(float min, float max)
{
	return min + static_cast <float> (rand()) / (static_cast <float> (RAND_MAX / (max - min)));
}

void ShowPRTDamage(int attacker, int client, int damage, bool crit, bool late = false)
{
	if(!IsValidClient(attacker, false))
		return;

	QAngle ang;
	Vector pos, pos2, fwd, right, temppos;
	float dist, d, dif;
	int ent, l, count, dmgnums[8];
	char buffer[128], buffer2[128];

	count = 0;

	while(damage > 0)
	{
		dmgnums[count++] = damage % 10;
		damage /= 10;
	}

	int count2 = count;

	CCSPlayerController* pAttacker = CCSPlayerController::FromSlot(attacker);
	if(!pAttacker) return;
	CCSPlayerPawn* pPawn = pAttacker->GetPlayerPawn();
	if(!pPawn) return;
	CCSPlayerController* pClient = CCSPlayerController::FromSlot(client);
	if(!pClient) return;
	CCSPlayerPawn* pClientPawn = pClient->GetPlayerPawn();
	if(!pClientPawn) return;
	ang = pPawn->m_angEyeAngles();
	pos2 = pPawn->GetAbsOrigin();

	if(late)
		pos = g_fPlayerPosLate[client];
	else
		pos = pClientPawn->GetAbsOrigin();
	
	AngleVectors(pPawn->GetAbsRotation(), &fwd, &right, NULL);

	l = RoundToCeil(float(count) / 2.0);
	int l2 = RoundToCeil(float(count) / 2.0);
	dist = pos2.DistTo(pos);
	if(dist > 700.0)
		d = dist / 700.0 * 6.0;
	else
		d = 6.0;

	pos.x += right.x * d * l * GetRandomFloat(-0.5, 1.0);
	pos.y += right.y * d * l * GetRandomFloat(-0.5, 1.0);
	CPlayer_MovementServices_Humanoid* m_pMovementServices = (CPlayer_MovementServices_Humanoid*)pClientPawn->m_pMovementServices();
	if(m_pMovementServices && m_pMovementServices->m_bDucked())
		if(crit)
			pos.z += 45.0 + GetRandomFloat(0.0, 10.0);
		else
			pos.z += 25.0 + GetRandomFloat(0.0, 20.0);
	else
		if(crit)
			pos.z += 60.0 + GetRandomFloat(0.0, 10.0);
		else
			pos.z += 35.0 + GetRandomFloat(0.0, 20.0);
	
	dif = g_fDistance;
	for(int i = count - 1; i >= 0; i-- )
	{
		temppos = pos;

		temppos.x -= fwd.x * dif + right.x * d * l;
		temppos.y -= fwd.y * dif + right.y * d * l;

		CParticleSystem* pEnt = (CParticleSystem*)g_pUtils->CreateEntityByName("info_particle_system", -1);
		CHandle<CParticleSystem> hEnt = pEnt->GetHandle();
		g_vPRTDamage[attacker].push_back(hEnt);
		g_SMAPI->Format(buffer, sizeof(buffer), "particles/kolka/fortnite_dmg_v2/kolka_damage_%i_%s%s", dmgnums[i], (l-- > 0 ? "fl" : "fr"), (crit ? "_crit" : ""));
		g_SMAPI->Format(buffer2, sizeof(buffer2), "%s.vpcf", buffer);
		pEnt->m_bStartActive(true);
		pEnt->m_iszEffectName(buffer2);
		g_pUtils->TeleportEntity(pEnt, &temppos, &ang, nullptr);
		g_pUtils->DispatchSpawn(pEnt, nullptr);
		g_pUtils->CreateTimer(0.5f, [attacker, hEnt, temppos, ang, buffer](){
			CParticleSystem* pEnt = hEnt;
			if(pEnt)
			{
				pEnt->m_bStartActive(false);
				g_pUtils->TeleportEntity(pEnt, nullptr, nullptr, nullptr);
				g_pUtils->RemoveEntity(pEnt);
			}
			g_vPRTDamage[attacker].erase(std::remove(g_vPRTDamage[attacker].begin(), g_vPRTDamage[attacker].end(), hEnt), g_vPRTDamage[attacker].end());
			CParticleSystem* pEntChilder = (CParticleSystem*)g_pUtils->CreateEntityByName("info_particle_system", -1);
			CHandle<CParticleSystem> hEntChilder = pEntChilder->GetHandle();
			g_vPRTDamage[attacker].push_back(hEntChilder);
			char buffer2[128];
			g_SMAPI->Format(buffer2, sizeof(buffer2), "%s_child.vpcf", buffer);
			pEntChilder->m_bStartActive(true);
			pEntChilder->m_iszEffectName(buffer2);
			g_pUtils->TeleportEntity(pEntChilder, &temppos, &ang, nullptr);
			g_pUtils->DispatchSpawn(pEntChilder, nullptr);
			g_pUtils->CreateTimer(2.5f, [attacker, hEntChilder](){
				CParticleSystem* pEntChilder = hEntChilder;
				if(pEntChilder)
				{
					g_pUtils->RemoveEntity(pEntChilder);
				}
				g_vPRTDamage[attacker].erase(std::remove(g_vPRTDamage[attacker].begin(), g_vPRTDamage[attacker].end(), hEntChilder), g_vPRTDamage[attacker].end());
				return -1.0f;
			});
			return -1.0f;
		});
	}
}

void OnPlayerHurt(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	int attacker, client, damage;
	HitGroup_t hitgroup;
	const char* sWeapon;
	
	attacker = pEvent->GetInt("attacker");
	if(!IsValidClient(attacker, false)) return;
	if((g_bFreeAccess || g_bHasAccess[attacker]) && g_bEnable[attacker])
	{
		client = pEvent->GetInt("userid");
		if(client < 0 || client >= 64) return;
		damage = pEvent->GetInt("dmg_health");
		hitgroup = static_cast<HitGroup_t>(pEvent->GetInt("hitgroup"));
		sWeapon = pEvent->GetString("weapon");
		
		if(attacker == client || g_pPlayers->IsFakeClient(attacker))
			return;

		if(!strcmp(sWeapon, "xm1014") || !strcmp(sWeapon, "nova") || !strcmp(sWeapon, "mag7") || !strcmp(sWeapon, "sawedoff"))
		{
			if(!g_bIsFired[attacker])
			{
				g_pUtils->CreateTimer(0.1f, [attacker](){
					g_bIsFired[attacker] = false;
					for(int i = 0; i < 64; i++)
					{
						if(!IsValidClient(i, false)) continue;
						
						if(g_iTotalSGDamage[attacker][i] != 0)
						{
							ShowPRTDamage(attacker, i, g_iTotalSGDamage[attacker][i], g_bIsCrit[attacker][i], true);
							g_iTotalSGDamage[attacker][i] = 0;
							g_bIsCrit[attacker][i] = false;
						}
					}
					return -1.0f;
				});
				
				g_bIsFired[attacker] = true;
				g_iTotalSGDamage[attacker][client] = damage;
			}
			else
				g_iTotalSGDamage[attacker][client] += damage;
			
			if(hitgroup == HITGROUP_HEAD)
				g_bIsCrit[attacker][client] = true;
			CCSPlayerController* pAttacker = CCSPlayerController::FromSlot(client);
			if(!pAttacker) return;
			CCSPlayerPawn* pPawn = pAttacker->GetPlayerPawn();
			if(!pPawn) return;
			g_fPlayerPosLate[client] = pPawn->GetAbsOrigin();
		}
		else
			ShowPRTDamage(attacker, client, damage, (hitgroup == HITGROUP_HEAD));
	}
}

std::vector<std::string> split(std::string s, std::string delimiter) {
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    std::string token;
    std::vector<std::string> res;

    while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos) {
        token = s.substr (pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back (token);
    }

    res.push_back (s.substr (pos_start));
    return res;
}

bool GetClientFN(int iSlot)
{
	CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
	if (!pController)
		return true;
	uint32 m_steamID = pController->m_steamID();
	if(m_steamID == 0)
		return true;
	return g_hKVData->GetBool(std::to_string(m_steamID).c_str(), true);
}

void SaveClientFH(int iSlot)
{
	CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
	if (!pController)
		return;
	uint32 m_steamID = pController->m_steamID();
	if(m_steamID == 0)
		return;
	g_hKVData->SetBool(std::to_string(m_steamID).c_str(), g_bEnable[iSlot]);
	g_hKVData->SaveToFile(g_pFullFileSystem, "addons/data/fh_data.ini");
}

void LoadConfig()
{
	KeyValues* g_kvSettings = new KeyValues("Settings");
	char szPath[256];
	g_SMAPI->Format(szPath, sizeof(szPath), "addons/configs/fortnite_hits.ini");
	if (!g_kvSettings->LoadFromFile(g_pFullFileSystem, szPath))
	{
		g_pUtils->ErrorLog("[%s] Failed to load %s", g_PLAPI->GetLogTag(), szPath);
		return;
	}
	g_fDistance = g_kvSettings->GetFloat("distance", 40.0);
	g_bFreeAccess = g_kvSettings->GetBool("free_access", true);

	const char* szCommands = g_kvSettings->GetString("commands");
	std::vector<std::string> vecCommands = split(std::string(szCommands), ";");
	g_pUtils->RegCommand(g_PLID, {}, vecCommands, [](int iSlot, const char* szContent){
		if(g_bHasAccess[iSlot] || g_bFreeAccess)
		{
			g_bEnable[iSlot] = !g_bEnable[iSlot];
			if(g_bEnable[iSlot])
				g_pUtils->PrintToChat(iSlot, g_vecPhrases["FH_Enable"].c_str());
			else
				g_pUtils->PrintToChat(iSlot, g_vecPhrases["FH_Disable"].c_str());
			SaveClientFH(iSlot);
		}
		else
			g_pUtils->PrintToChat(iSlot, g_vecPhrases["FH_NoAccess"].c_str());
		return true;
	});
}

void LoadTranslations()
{
	KeyValues::AutoDelete g_kvPhrases("Phrases");
	const char *pszPath = "addons/translations/fortnite_hits.phrases.txt";
	if (!g_kvPhrases->LoadFromFile(g_pFullFileSystem, pszPath))
	{
		Warning("Failed to load %s\n", pszPath);
		return;
	}

	std::string szLanguage = std::string(g_pUtils->GetLanguage());
	const char* g_pszLanguage = szLanguage.c_str();
	for (KeyValues *pKey = g_kvPhrases->GetFirstTrueSubKey(); pKey; pKey = pKey->GetNextTrueSubKey())
		g_vecPhrases[std::string(pKey->GetName())] = std::string(pKey->GetString(g_pszLanguage));
}

bool LoadData()
{
	g_hKVData = new KeyValues("Data");

	const char *pszPath = "addons/data/fh_data.ini";

	if (!g_hKVData->LoadFromFile(g_pFullFileSystem, pszPath))
	{
		g_pUtils->ErrorLog("[%s] Failed to load %s", g_PLAPI->GetLogTag(), pszPath);
		return false;
	}

	return true;
}

void OnClientAuthorized(int iSlot, uint64 iSteamID64)
{
	g_bHasAccess[iSlot] = false;
	g_bEnable[iSlot] = GetClientFN(iSlot);
}

void Fortnite_Hits::AllPluginsLoaded()
{
	char error[64];
	int ret;
	g_pUtils = (IUtilsApi *)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_SMAPI->Format(error, sizeof(error), "Missing Utils system plugin");
		ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	g_pPlayers = (IPlayersApi *)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_SMAPI->Format(error, sizeof(error), "Missing Players system plugin");
		ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	g_pUtils->StartupServer(g_PLID, StartupServer);
	g_pUtils->HookEvent(g_PLID, "player_hurt", OnPlayerHurt);
	g_pPlayers->HookOnClientAuthorized(g_PLID, OnClientAuthorized);

	LoadData();
	LoadConfig();
	LoadTranslations();
}

///////////////////////////////////////
const char* Fortnite_Hits::GetLicense()
{
	return "GPL";
}

const char* Fortnite_Hits::GetVersion()
{
	return "1.1.1";
}

const char* Fortnite_Hits::GetDate()
{
	return __DATE__;
}

const char *Fortnite_Hits::GetLogTag()
{
	return "Fortnite_Hits";
}

const char* Fortnite_Hits::GetAuthor()
{
	return "Pisex & kolkazadrot";
}

const char* Fortnite_Hits::GetDescription()
{
	return "Fortnite Hits";
}

const char* Fortnite_Hits::GetName()
{
	return "Fortnite Hits";
}

const char* Fortnite_Hits::GetURL()
{
	return "https://discord.gg/g798xERK5Y";
}
