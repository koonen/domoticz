#include "stdafx.h"
#include "WebServer.h"
#include "WebServerHelper.h"
#include <iostream>
#include <fstream>
#include <stdarg.h>
#include "mainworker.h"
#include "Helper.h"
#include "EventSystem.h"
#include "HTMLSanitizer.h"
#include "dzVents.h"
#include "../httpclient/HTTPClient.h"
#include "../hardware/hardwaretypes.h"

#include "../hardware/1Wire.h"
#include "../hardware/AccuWeather.h"
#include "../hardware/AirconWithMe.h"
#include "../hardware/Buienradar.h"
#include "../hardware/DarkSky.h"
#include "../hardware/VisualCrossing.h"
#include "../hardware/eHouseTCP.h"
#include "../hardware/EnOceanESP2.h"
#include "../hardware/EnOceanESP3.h"
#include "../hardware/EnphaseAPI.h"
#include "../hardware/AlfenEve.h"
#ifdef WITH_GPIO
#include "../hardware/Gpio.h"
#include "../hardware/GpioPin.h"
#endif // WITH_GPIO
#include "../hardware/HEOS.h"
#include "../hardware/Kodi.h"
#include "../hardware/Limitless.h"
#include "../hardware/LogitechMediaServer.h"
#include "../hardware/Meteorologisk.h"
#include "../hardware/MySensorsBase.h"
#include "../hardware/OpenWeatherMap.h"
#include "../hardware/OTGWBase.h"
#include "../hardware/RFLinkBase.h"
#include "../hardware/RFXBase.h"
#include "../hardware/SysfsGpio.h"
#include "../hardware/Tellstick.h"
#include "../hardware/USBtin.h"
#include "../hardware/USBtin_MultiblocV8.h"
#include "../hardware/Wunderground.h"
#include "../hardware/ZiBlueBase.h"

#include "../webserver/Base64.h"
#include "../smtpclient/SMTPClient.h"
#include <json/json.h>
#include "../main/json_helper.h"
#include "Logger.h"
#include "SQLHelper.h"
#include "../push/BasePush.h"
#include <algorithm>
#ifdef ENABLE_PYTHON
#include "../hardware/plugins/Plugins.h"
#endif

#ifndef WIN32
#include <sys/utsname.h>
#include <dirent.h>
#else
#include "../msbuild/WindowsHelper.h"
#include "dirent_windows.h"
#endif
#include "../notifications/NotificationHelper.h"
#include "../main/LuaHandler.h"

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#define round(a) (int)(a + .5)

extern std::string szStartupFolder;
extern std::string szUserDataFolder;
extern std::string szWWWFolder;

extern std::string szAppVersion;
extern int iAppRevision;
extern std::string szAppHash;
extern std::string szAppDate;
extern std::string szPyVersion;

extern bool g_bUseUpdater;

extern time_t m_StartTime;

struct _tGuiLanguage
{
	const char* szShort;
	const char* szLong;
};

namespace
{
	constexpr std::array<std::pair<const char*, const char*>, 36> guiLanguage{ {
		{ "en", "English" }, { "sq", "Albanian" }, { "ar", "Arabic" }, { "bs", "Bosnian" }, { "bg", "Bulgarian" }, { "ca", "Catalan" },
		{ "zh", "Chinese" }, { "cs", "Czech" }, { "da", "Danish" }, { "nl", "Dutch" }, { "et", "Estonian" }, { "de", "German" },
		{ "el", "Greek" }, { "fr", "French" }, { "fi", "Finnish" }, { "he", "Hebrew" }, { "hu", "Hungarian" }, { "is", "Icelandic" },
		{ "it", "Italian" }, { "lt", "Lithuanian" }, { "lv", "Latvian" }, { "mk", "Macedonian" }, { "no", "Norwegian" }, { "fa", "Persian" },
		{ "pl", "Polish" }, { "pt", "Portuguese" }, { "ro", "Romanian" }, { "ru", "Russian" }, { "sr", "Serbian" }, { "sk", "Slovak" },
		{ "sl", "Slovenian" }, { "es", "Spanish" }, { "sv", "Swedish" }, { "zh_TW", "Taiwanese" }, { "tr", "Turkish" }, { "uk", "Ukrainian" },
		} };
} // namespace

extern http::server::CWebServerHelper m_webservers;

namespace http
{
	namespace server
	{

		CWebServer::CWebServer()
		{
			m_pWebEm = nullptr;
			m_bDoStop = false;
			m_failcount = 0;
		}

		CWebServer::~CWebServer()
		{
			// RK, we call StopServer() instead of just deleting m_pWebEm. The Do_Work thread might still be accessing that object
			StopServer();
		}

		void CWebServer::Do_Work()
		{
			bool exception_thrown = false;
			while (!m_bDoStop)
			{
				exception_thrown = false;
				try
				{
					if (m_pWebEm)
					{
						m_pWebEm->Run();
					}
				}
				catch (std::exception& e)
				{
					_log.Log(LOG_ERROR, "WebServer(%s) exception occurred : '%s'", m_server_alias.c_str(), e.what());
					exception_thrown = true;
				}
				catch (...)
				{
					_log.Log(LOG_ERROR, "WebServer(%s) unknown exception occurred", m_server_alias.c_str());
					exception_thrown = true;
				}
				if (exception_thrown)
				{
					_log.Log(LOG_STATUS, "WebServer(%s) restart server in 5 seconds", m_server_alias.c_str());
					sleep_milliseconds(5000); // prevents from an exception flood
					continue;
				}
				break;
			}
			_log.Log(LOG_STATUS, "WebServer(%s) stopped", m_server_alias.c_str());
		}

		void CWebServer::ReloadCustomSwitchIcons()
		{
			m_custom_light_icons.clear();
			m_custom_light_icons_lookup.clear();
			std::string sLine;

			// First get them from the switch_icons.txt file
			std::ifstream infile;
			std::string switchlightsfile = szWWWFolder + "/switch_icons.txt";
			infile.open(switchlightsfile.c_str());
			if (infile.is_open())
			{
				int index = 0;
				while (!infile.eof())
				{
					getline(infile, sLine);
					if (!sLine.empty())
					{
						std::vector<std::string> results;
						StringSplit(sLine, ";", results);
						if (results.size() == 3)
						{
							_tCustomIcon cImage;
							cImage.idx = index++;
							cImage.RootFile = results[0];
							cImage.Title = results[1];
							cImage.Description = results[2];
							m_custom_light_icons.push_back(cImage);
							m_custom_light_icons_lookup[cImage.idx] = (int)m_custom_light_icons.size() - 1;
						}
					}
				}
				infile.close();
			}
			// Now get them from the database (idx 100+)
			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT ID,Base,Name,Description FROM CustomImages");
			if (!result.empty())
			{
				int ii = 0;
				for (const auto& sd : result)
				{
					int ID = atoi(sd[0].c_str());

					_tCustomIcon cImage;
					cImage.idx = 100 + ID;
					cImage.RootFile = sd[1];
					cImage.Title = sd[2];
					cImage.Description = sd[3];

					std::string IconFile16 = cImage.RootFile + ".png";
					std::string IconFile48On = cImage.RootFile + "48_On.png";
					std::string IconFile48Off = cImage.RootFile + "48_Off.png";

					std::map<std::string, std::string> _dbImageFiles;
					_dbImageFiles["IconSmall"] = szWWWFolder + "/images/" + IconFile16;
					_dbImageFiles["IconOn"] = szWWWFolder + "/images/" + IconFile48On;
					_dbImageFiles["IconOff"] = szWWWFolder + "/images/" + IconFile48Off;

					// Check if files are on disk, else add them
					for (const auto& db : _dbImageFiles)
					{
						std::string TableField = db.first;
						std::string IconFile = db.second;

						if (!file_exist(IconFile.c_str()))
						{
							// Does not exists, extract it from the database and add it
							std::vector<std::vector<std::string>> result2;
							result2 = m_sql.safe_queryBlob("SELECT %s FROM CustomImages WHERE ID=%d", TableField.c_str(), ID);
							if (!result2.empty())
							{
								std::ofstream file;
								file.open(IconFile.c_str(), std::ios::out | std::ios::binary);
								if (!file.is_open())
									return;

								file << result2[0][0];
								file.close();
							}
						}
					}

					m_custom_light_icons.push_back(cImage);
					m_custom_light_icons_lookup[cImage.idx] = (int)m_custom_light_icons.size() - 1;
					ii++;
				}
			}
		}

		bool CWebServer::StartServer(server_settings& settings, const std::string& serverpath, const bool bIgnoreUsernamePassword)
		{
			if (!settings.is_enabled())
				return true;

			m_server_alias = (settings.is_secure() == true) ? "SSL" : "HTTP";

			std::string sRealm = (settings.is_secure() == true) ? "https://" : "http://";

			if (!settings.vhostname.empty())
				sRealm += settings.vhostname;
			else
				sRealm += (settings.listening_address == "::") ? "domoticz.local" : settings.listening_address;
			if (settings.listening_port != "80" || settings.listening_port != "443")
				sRealm += ":" + settings.listening_port;
			sRealm += "/";

			ReloadCustomSwitchIcons();

			int tries = 0;
			bool exception = false;

			_log.Debug(DEBUG_WEBSERVER, "CWebServer::StartServer() : settings : %s", settings.to_string().c_str());
			_log.Debug(DEBUG_AUTH, "CWebServer::StartServer() : IAM settings : %s", m_iamsettings.to_string().c_str());
			do
			{
				try
				{
					exception = false;
					m_pWebEm = new http::server::cWebem(settings, serverpath);
				}
				catch (std::exception& e)
				{
					exception = true;
					switch (tries)
					{
					case 0:
						_log.Log(LOG_STATUS, "WebServer(%s) startup failed on address %s with port: %s: %s, trying ::", m_server_alias.c_str(),
							settings.listening_address.c_str(), settings.listening_port.c_str(), e.what());
						settings.listening_address = "::";
						break;
					case 1:
						_log.Log(LOG_STATUS, "WebServer(%s) startup failed on address %s with port: %s: %s, trying 0.0.0.0", m_server_alias.c_str(),
							settings.listening_address.c_str(), settings.listening_port.c_str(), e.what());
						settings.listening_address = "0.0.0.0";
						break;
					case 2:
						_log.Log(LOG_ERROR, "WebServer(%s) startup failed on address %s with port: %s: %s", m_server_alias.c_str(), settings.listening_address.c_str(),
							settings.listening_port.c_str(), e.what());
						if (atoi(settings.listening_port.c_str()) < 1024)
							_log.Log(LOG_ERROR, "WebServer(%s) check privileges for opening ports below 1024", m_server_alias.c_str());
						else
							_log.Log(LOG_ERROR, "WebServer(%s) check if no other application is using port: %s", m_server_alias.c_str(),
								settings.listening_port.c_str());
						return false;
					}
					tries++;
				}
			} while (exception);

			_log.Log(LOG_STATUS, "WebServer(%s) started on address: %s with port %s", m_server_alias.c_str(), settings.listening_address.c_str(), settings.listening_port.c_str());

			m_pWebEm->SetDigistRealm(sRealm);
			m_pWebEm->SetSessionStore(this);

			LoadUsers();

			std::string TrustedNetworks;
			if (m_sql.GetPreferencesVar("WebLocalNetworks", TrustedNetworks))
			{
				std::vector<std::string> strarray;
				StringSplit(TrustedNetworks, ";", strarray);
				for (const auto& str : strarray)
					m_pWebEm->AddTrustedNetworks(str);
			}
			if (bIgnoreUsernamePassword)
			{
				m_pWebEm->AddTrustedNetworks("0.0.0.0/0");	// IPv4
				m_pWebEm->AddTrustedNetworks("::");	// IPv6
				_log.Log(LOG_ERROR, "SECURITY RISK! Allowing access without username/password as all incoming traffic is considered trusted! Change admin password asap and restart Domoticz!");

				if (m_users.empty())
				{
					AddUser(99999, "tmpadmin", "tmpadmin", "", (_eUserRights)URIGHTS_ADMIN, 0x1F);
					_log.Debug(DEBUG_AUTH, "[Start server] Added tmpadmin User as no active Users where found!");
				}
			}

			// register callbacks
			if (m_iamsettings.is_enabled())
			{
				m_pWebEm->RegisterPageCode(
					m_iamsettings.auth_url.c_str(), [this](auto&& session, auto&& req, auto&& rep) { GetOauth2AuthCode(session, req, rep); }, true);
				m_pWebEm->RegisterPageCode(
					m_iamsettings.token_url.c_str(), [this](auto&& session, auto&& req, auto&& rep) { PostOauth2AccessToken(session, req, rep); }, true);
				m_pWebEm->RegisterPageCode(
					m_iamsettings.discovery_url.c_str(), [this](auto&& session, auto&& req, auto&& rep) { GetOpenIDConfiguration(session, req, rep); }, true);
			}

			m_pWebEm->RegisterPageCode("/json.htm", [this](auto&& session, auto&& req, auto&& rep) { GetJSonPage(session, req, rep); });
			// These 'Pages' should probably be 'moved' to become Command codes handled by the 'json.htm API', so we get all API calls through one entry point
			// And why .php or .cgi while all these commands are NOT handled by a PHP or CGI processor but by Domoticz ?? Legacy? Rename these?
			m_pWebEm->RegisterPageCode("/backupdatabase.php", [this](auto&& session, auto&& req, auto&& rep) { GetDatabaseBackup(session, req, rep); });
			m_pWebEm->RegisterPageCode("/camsnapshot.jpg", [this](auto&& session, auto&& req, auto&& rep) { GetCameraSnapshot(session, req, rep); });
			m_pWebEm->RegisterPageCode("/raspberry.cgi", [this](auto&& session, auto&& req, auto&& rep) { GetInternalCameraSnapshot(session, req, rep); });
			m_pWebEm->RegisterPageCode("/uvccapture.cgi", [this](auto&& session, auto&& req, auto&& rep) { GetInternalCameraSnapshot(session, req, rep); });
			// Maybe handle these differently? (Or remove)
			m_pWebEm->RegisterPageCode("/images/floorplans/plan", [this](auto&& session, auto&& req, auto&& rep) { GetFloorplanImage(session, req, rep); });
			m_pWebEm->RegisterPageCode("/service-worker.js", [this](auto&& session, auto&& req, auto&& rep) { GetServiceWorker(session, req, rep); });

			// End of 'Pages' to be moved...

			m_pWebEm->RegisterActionCode("setrfxcommode", [this](auto&& session, auto&& req, auto&& redirect_uri) { SetRFXCOMMode(session, req, redirect_uri); });
			m_pWebEm->RegisterActionCode("rfxupgradefirmware", [this](auto&& session, auto&& req, auto&& redirect_uri) { RFXComUpgradeFirmware(session, req, redirect_uri); });
			m_pWebEm->RegisterActionCode("setrego6xxtype", [this](auto&& session, auto&& req, auto&& redirect_uri) { SetRego6XXType(session, req, redirect_uri); });
			m_pWebEm->RegisterActionCode("sets0metertype", [this](auto&& session, auto&& req, auto&& redirect_uri) { SetS0MeterType(session, req, redirect_uri); });
			m_pWebEm->RegisterActionCode("setlimitlesstype", [this](auto&& session, auto&& req, auto&& redirect_uri) { SetLimitlessType(session, req, redirect_uri); });

			m_pWebEm->RegisterActionCode("uploadfloorplanimage", [this](auto&& session, auto&& req, auto&& redirect_uri) { UploadFloorplanImage(session, req, redirect_uri); });

			m_pWebEm->RegisterActionCode("setopenthermsettings", [this](auto&& session, auto&& req, auto&& redirect_uri) { SetOpenThermSettings(session, req, redirect_uri); });

			m_pWebEm->RegisterActionCode("reloadpiface", [this](auto&& session, auto&& req, auto&& redirect_uri) { ReloadPiFace(session, req, redirect_uri); });
			m_pWebEm->RegisterActionCode("restoredatabase", [this](auto&& session, auto&& req, auto&& redirect_uri) { RestoreDatabase(session, req, redirect_uri); });
			m_pWebEm->RegisterActionCode("sbfspotimportolddata", [this](auto&& session, auto&& req, auto&& redirect_uri) { SBFSpotImportOldData(session, req, redirect_uri); });

			// Commands that do NOT require authentication
			RegisterCommandCode("gettimertypes", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetTimerTypes(session, req, root); }, true);
			RegisterCommandCode("getlanguages", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetLanguages(session, req, root); }, true);
			RegisterCommandCode("getswitchtypes", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetSwitchTypes(session, req, root); }, true);
			RegisterCommandCode("getmetertypes", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetMeterTypes(session, req, root); }, true);
			RegisterCommandCode("getthemes", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetThemes(session, req, root); }, true);
			RegisterCommandCode("gettitle", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetTitle(session, req, root); }, true);
			RegisterCommandCode("logincheck", [this](auto&& session, auto&& req, auto&& root) { Cmd_LoginCheck(session, req, root); }, true);

			RegisterCommandCode("getversion", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetVersion(session, req, root); }, true);
			RegisterCommandCode("getauth", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetAuth(session, req, root); }, true);
			RegisterCommandCode("getuptime", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetUptime(session, req, root); }, true);
			RegisterCommandCode("getconfig", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetConfig(session, req, root); }, true);

			RegisterCommandCode("rfxfirmwaregetpercentage", [this](auto&& session, auto&& req, auto&& root) { Cmd_RFXComGetFirmwarePercentage(session, req, root); }, true);

			// Commands that require authentication
			RegisterCommandCode("sendopenthermcommand", [this](auto&& session, auto&& req, auto&& root) { Cmd_SendOpenThermCommand(session, req, root); });

			RegisterCommandCode("storesettings", [this](auto&& session, auto&& req, auto&& root) { Cmd_PostSettings(session, req, root); });
			RegisterCommandCode("getlog", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetLog(session, req, root); });
			RegisterCommandCode("clearlog", [this](auto&& session, auto&& req, auto&& root) { Cmd_ClearLog(session, req, root); });
			RegisterCommandCode("gethardwaretypes", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetHardwareTypes(session, req, root); });
			RegisterCommandCode("addhardware", [this](auto&& session, auto&& req, auto&& root) { Cmd_AddHardware(session, req, root); });
			RegisterCommandCode("updatehardware", [this](auto&& session, auto&& req, auto&& root) { Cmd_UpdateHardware(session, req, root); });
			RegisterCommandCode("deletehardware", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteHardware(session, req, root); });

			RegisterCommandCode("addcamera", [this](auto&& session, auto&& req, auto&& root) { Cmd_AddCamera(session, req, root); });
			RegisterCommandCode("updatecamera", [this](auto&& session, auto&& req, auto&& root) { Cmd_UpdateCamera(session, req, root); });
			RegisterCommandCode("deletecamera", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteCamera(session, req, root); });

			RegisterCommandCode("wolgetnodes", [this](auto&& session, auto&& req, auto&& root) { Cmd_WOLGetNodes(session, req, root); });
			RegisterCommandCode("woladdnode", [this](auto&& session, auto&& req, auto&& root) { Cmd_WOLAddNode(session, req, root); });
			RegisterCommandCode("wolupdatenode", [this](auto&& session, auto&& req, auto&& root) { Cmd_WOLUpdateNode(session, req, root); });
			RegisterCommandCode("wolremovenode", [this](auto&& session, auto&& req, auto&& root) { Cmd_WOLRemoveNode(session, req, root); });
			RegisterCommandCode("wolclearnodes", [this](auto&& session, auto&& req, auto&& root) { Cmd_WOLClearNodes(session, req, root); });

			RegisterCommandCode("mysensorsgetnodes", [this](auto&& session, auto&& req, auto&& root) { Cmd_MySensorsGetNodes(session, req, root); });
			RegisterCommandCode("mysensorsgetchilds", [this](auto&& session, auto&& req, auto&& root) { Cmd_MySensorsGetChilds(session, req, root); });
			RegisterCommandCode("mysensorsupdatenode", [this](auto&& session, auto&& req, auto&& root) { Cmd_MySensorsUpdateNode(session, req, root); });
			RegisterCommandCode("mysensorsremovenode", [this](auto&& session, auto&& req, auto&& root) { Cmd_MySensorsRemoveNode(session, req, root); });
			RegisterCommandCode("mysensorsremovechild", [this](auto&& session, auto&& req, auto&& root) { Cmd_MySensorsRemoveChild(session, req, root); });
			RegisterCommandCode("mysensorsupdatechild", [this](auto&& session, auto&& req, auto&& root) { Cmd_MySensorsUpdateChild(session, req, root); });

			RegisterCommandCode("pingersetmode", [this](auto&& session, auto&& req, auto&& root) { Cmd_PingerSetMode(session, req, root); });
			RegisterCommandCode("pingergetnodes", [this](auto&& session, auto&& req, auto&& root) { Cmd_PingerGetNodes(session, req, root); });
			RegisterCommandCode("pingeraddnode", [this](auto&& session, auto&& req, auto&& root) { Cmd_PingerAddNode(session, req, root); });
			RegisterCommandCode("pingerupdatenode", [this](auto&& session, auto&& req, auto&& root) { Cmd_PingerUpdateNode(session, req, root); });
			RegisterCommandCode("pingerremovenode", [this](auto&& session, auto&& req, auto&& root) { Cmd_PingerRemoveNode(session, req, root); });
			RegisterCommandCode("pingerclearnodes", [this](auto&& session, auto&& req, auto&& root) { Cmd_PingerClearNodes(session, req, root); });

			RegisterCommandCode("kodisetmode", [this](auto&& session, auto&& req, auto&& root) { Cmd_KodiSetMode(session, req, root); });
			RegisterCommandCode("kodigetnodes", [this](auto&& session, auto&& req, auto&& root) { Cmd_KodiGetNodes(session, req, root); });
			RegisterCommandCode("kodiaddnode", [this](auto&& session, auto&& req, auto&& root) { Cmd_KodiAddNode(session, req, root); });
			RegisterCommandCode("kodiupdatenode", [this](auto&& session, auto&& req, auto&& root) { Cmd_KodiUpdateNode(session, req, root); });
			RegisterCommandCode("kodiremovenode", [this](auto&& session, auto&& req, auto&& root) { Cmd_KodiRemoveNode(session, req, root); });
			RegisterCommandCode("kodiclearnodes", [this](auto&& session, auto&& req, auto&& root) { Cmd_KodiClearNodes(session, req, root); });
			RegisterCommandCode("kodimediacommand", [this](auto&& session, auto&& req, auto&& root) { Cmd_KodiMediaCommand(session, req, root); });

			RegisterCommandCode("panasonicsetmode", [this](auto&& session, auto&& req, auto&& root) { Cmd_PanasonicSetMode(session, req, root); });
			RegisterCommandCode("panasonicgetnodes", [this](auto&& session, auto&& req, auto&& root) { Cmd_PanasonicGetNodes(session, req, root); });
			RegisterCommandCode("panasonicaddnode", [this](auto&& session, auto&& req, auto&& root) { Cmd_PanasonicAddNode(session, req, root); });
			RegisterCommandCode("panasonicupdatenode", [this](auto&& session, auto&& req, auto&& root) { Cmd_PanasonicUpdateNode(session, req, root); });
			RegisterCommandCode("panasonicremovenode", [this](auto&& session, auto&& req, auto&& root) { Cmd_PanasonicRemoveNode(session, req, root); });
			RegisterCommandCode("panasonicclearnodes", [this](auto&& session, auto&& req, auto&& root) { Cmd_PanasonicClearNodes(session, req, root); });
			RegisterCommandCode("panasonicmediacommand", [this](auto&& session, auto&& req, auto&& root) { Cmd_PanasonicMediaCommand(session, req, root); });

			RegisterCommandCode("heossetmode", [this](auto&& session, auto&& req, auto&& root) { Cmd_HEOSSetMode(session, req, root); });
			RegisterCommandCode("heosmediacommand", [this](auto&& session, auto&& req, auto&& root) { Cmd_HEOSMediaCommand(session, req, root); });

			RegisterCommandCode("onkyoeiscpcommand", [this](auto&& session, auto&& req, auto&& root) { Cmd_OnkyoEiscpCommand(session, req, root); });

			RegisterCommandCode("bleboxsetmode", [this](auto&& session, auto&& req, auto&& root) { Cmd_BleBoxSetMode(session, req, root); });
			RegisterCommandCode("bleboxgetnodes", [this](auto&& session, auto&& req, auto&& root) { Cmd_BleBoxGetNodes(session, req, root); });
			RegisterCommandCode("bleboxaddnode", [this](auto&& session, auto&& req, auto&& root) { Cmd_BleBoxAddNode(session, req, root); });
			RegisterCommandCode("bleboxremovenode", [this](auto&& session, auto&& req, auto&& root) { Cmd_BleBoxRemoveNode(session, req, root); });
			RegisterCommandCode("bleboxclearnodes", [this](auto&& session, auto&& req, auto&& root) { Cmd_BleBoxClearNodes(session, req, root); });
			RegisterCommandCode("bleboxautosearchingnodes", [this](auto&& session, auto&& req, auto&& root) { Cmd_BleBoxAutoSearchingNodes(session, req, root); });
			RegisterCommandCode("bleboxupdatefirmware", [this](auto&& session, auto&& req, auto&& root) { Cmd_BleBoxUpdateFirmware(session, req, root); });

			RegisterCommandCode("lmssetmode", [this](auto&& session, auto&& req, auto&& root) { Cmd_LMSSetMode(session, req, root); });
			RegisterCommandCode("lmsgetnodes", [this](auto&& session, auto&& req, auto&& root) { Cmd_LMSGetNodes(session, req, root); });
			RegisterCommandCode("lmsgetplaylists", [this](auto&& session, auto&& req, auto&& root) { Cmd_LMSGetPlaylists(session, req, root); });
			RegisterCommandCode("lmsmediacommand", [this](auto&& session, auto&& req, auto&& root) { Cmd_LMSMediaCommand(session, req, root); });
			RegisterCommandCode("lmsdeleteunuseddevices", [this](auto&& session, auto&& req, auto&& root) { Cmd_LMSDeleteUnusedDevices(session, req, root); });

			RegisterCommandCode("savefibarolinkconfig", [this](auto&& session, auto&& req, auto&& root) { Cmd_SaveFibaroLinkConfig(session, req, root); });
			RegisterCommandCode("getfibarolinkconfig", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetFibaroLinkConfig(session, req, root); });
			RegisterCommandCode("getfibarolinks", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetFibaroLinks(session, req, root); });
			RegisterCommandCode("savefibarolink", [this](auto&& session, auto&& req, auto&& root) { Cmd_SaveFibaroLink(session, req, root); });
			RegisterCommandCode("deletefibarolink", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteFibaroLink(session, req, root); });

			RegisterCommandCode("saveinfluxlinkconfig", [this](auto&& session, auto&& req, auto&& root) { Cmd_SaveInfluxLinkConfig(session, req, root); });
			RegisterCommandCode("getinfluxlinkconfig", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetInfluxLinkConfig(session, req, root); });
			RegisterCommandCode("getinfluxlinks", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetInfluxLinks(session, req, root); });
			RegisterCommandCode("saveinfluxlink", [this](auto&& session, auto&& req, auto&& root) { Cmd_SaveInfluxLink(session, req, root); });
			RegisterCommandCode("deleteinfluxlink", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteInfluxLink(session, req, root); });

			RegisterCommandCode("savehttplinkconfig", [this](auto&& session, auto&& req, auto&& root) { Cmd_SaveHttpLinkConfig(session, req, root); });
			RegisterCommandCode("gethttplinkconfig", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetHttpLinkConfig(session, req, root); });
			RegisterCommandCode("gethttplinks", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetHttpLinks(session, req, root); });
			RegisterCommandCode("savehttplink", [this](auto&& session, auto&& req, auto&& root) { Cmd_SaveHttpLink(session, req, root); });
			RegisterCommandCode("deletehttplink", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteHttpLink(session, req, root); });

			RegisterCommandCode("savegooglepubsublinkconfig", [this](auto&& session, auto&& req, auto&& root) { Cmd_SaveGooglePubSubLinkConfig(session, req, root); });
			RegisterCommandCode("getgooglepubsublinkconfig", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetGooglePubSubLinkConfig(session, req, root); });
			RegisterCommandCode("getgooglepubsublinks", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetGooglePubSubLinks(session, req, root); });
			RegisterCommandCode("savegooglepubsublink", [this](auto&& session, auto&& req, auto&& root) { Cmd_SaveGooglePubSubLink(session, req, root); });
			RegisterCommandCode("deletegooglepubsublink", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteGooglePubSubLink(session, req, root); });

			RegisterCommandCode("getdevicevalueoptions", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetDeviceValueOptions(session, req, root); });
			RegisterCommandCode("getdevicevalueoptionwording", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetDeviceValueOptionWording(session, req, root); });

			RegisterCommandCode("adduservariable", [this](auto&& session, auto&& req, auto&& root) { Cmd_AddUserVariable(session, req, root); });
			RegisterCommandCode("updateuservariable", [this](auto&& session, auto&& req, auto&& root) { Cmd_UpdateUserVariable(session, req, root); });
			RegisterCommandCode("deleteuservariable", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteUserVariable(session, req, root); });
			RegisterCommandCode("getuservariables", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetUserVariables(session, req, root); });
			RegisterCommandCode("getuservariable", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetUserVariable(session, req, root); });

			RegisterCommandCode("allownewhardware", [this](auto&& session, auto&& req, auto&& root) { Cmd_AllowNewHardware(session, req, root); });

			RegisterCommandCode("addplan", [this](auto&& session, auto&& req, auto&& root) { Cmd_AddPlan(session, req, root); });
			RegisterCommandCode("updateplan", [this](auto&& session, auto&& req, auto&& root) { Cmd_UpdatePlan(session, req, root); });
			RegisterCommandCode("deleteplan", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeletePlan(session, req, root); });
			RegisterCommandCode("getunusedplandevices", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetUnusedPlanDevices(session, req, root); });
			RegisterCommandCode("addplanactivedevice", [this](auto&& session, auto&& req, auto&& root) { Cmd_AddPlanActiveDevice(session, req, root); });
			RegisterCommandCode("getplandevices", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetPlanDevices(session, req, root); });
			RegisterCommandCode("deleteplandevice", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeletePlanDevice(session, req, root); });
			RegisterCommandCode("setplandevicecoords", [this](auto&& session, auto&& req, auto&& root) { Cmd_SetPlanDeviceCoords(session, req, root); });
			RegisterCommandCode("deleteallplandevices", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteAllPlanDevices(session, req, root); });
			RegisterCommandCode("changeplanorder", [this](auto&& session, auto&& req, auto&& root) { Cmd_ChangePlanOrder(session, req, root); });
			RegisterCommandCode("changeplandeviceorder", [this](auto&& session, auto&& req, auto&& root) { Cmd_ChangePlanDeviceOrder(session, req, root); });

			RegisterCommandCode("gettimerplans", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetTimerPlans(session, req, root); });
			RegisterCommandCode("addtimerplan", [this](auto&& session, auto&& req, auto&& root) { Cmd_AddTimerPlan(session, req, root); });
			RegisterCommandCode("updatetimerplan", [this](auto&& session, auto&& req, auto&& root) { Cmd_UpdateTimerPlan(session, req, root); });
			RegisterCommandCode("deletetimerplan", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteTimerPlan(session, req, root); });
			RegisterCommandCode("duplicatetimerplan", [this](auto&& session, auto&& req, auto&& root) { Cmd_DuplicateTimerPlan(session, req, root); });

			RegisterCommandCode("getactualhistory", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetActualHistory(session, req, root); });
			RegisterCommandCode("getnewhistory", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetNewHistory(session, req, root); });

			RegisterCommandCode("getmyprofile", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetMyProfile(session, req, root); });
			RegisterCommandCode("updatemyprofile", [this](auto&& session, auto&& req, auto&& root) { Cmd_UpdateMyProfile(session, req, root); });

			RegisterCommandCode("getlocation", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetLocation(session, req, root); });
			RegisterCommandCode("getforecastconfig", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetForecastConfig(session, req, root); });
			RegisterCommandCode("sendnotification", [this](auto&& session, auto&& req, auto&& root) { Cmd_SendNotification(session, req, root); });
			RegisterCommandCode("emailcamerasnapshot", [this](auto&& session, auto&& req, auto&& root) { Cmd_EmailCameraSnapshot(session, req, root); });
			RegisterCommandCode("udevice", [this](auto&& session, auto&& req, auto&& root) { Cmd_UpdateDevice(session, req, root); });
			RegisterCommandCode("udevices", [this](auto&& session, auto&& req, auto&& root) { Cmd_UpdateDevices(session, req, root); });
			RegisterCommandCode("thermostatstate", [this](auto&& session, auto&& req, auto&& root) { Cmd_SetThermostatState(session, req, root); });
			RegisterCommandCode("system_shutdown", [this](auto&& session, auto&& req, auto&& root) { Cmd_SystemShutdown(session, req, root); });
			RegisterCommandCode("system_reboot", [this](auto&& session, auto&& req, auto&& root) { Cmd_SystemReboot(session, req, root); });
			RegisterCommandCode("execute_script", [this](auto&& session, auto&& req, auto&& root) { Cmd_ExcecuteScript(session, req, root); });
			RegisterCommandCode("getcosts", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetCosts(session, req, root); });
			RegisterCommandCode("checkforupdate", [this](auto&& session, auto&& req, auto&& root) { Cmd_CheckForUpdate(session, req, root); });
			RegisterCommandCode("downloadupdate", [this](auto&& session, auto&& req, auto&& root) { Cmd_DownloadUpdate(session, req, root); });
			RegisterCommandCode("downloadready", [this](auto&& session, auto&& req, auto&& root) { Cmd_DownloadReady(session, req, root); });
			RegisterCommandCode("update_application", [this](auto&& session, auto&& req, auto&& root) { Cmd_UpdateApplication(session, req, root); });
			RegisterCommandCode("deletedatapoint", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteDataPoint(session, req, root); });
			RegisterCommandCode("deletedaterange", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteDateRange(session, req, root); });
			RegisterCommandCode("customevent", [this](auto&& session, auto&& req, auto&& root) { Cmd_CustomEvent(session, req, root); });

			RegisterCommandCode("setactivetimerplan", [this](auto&& session, auto&& req, auto&& root) { Cmd_SetActiveTimerPlan(session, req, root); });
			RegisterCommandCode("addtimer", [this](auto&& session, auto&& req, auto&& root) { Cmd_AddTimer(session, req, root); });
			RegisterCommandCode("updatetimer", [this](auto&& session, auto&& req, auto&& root) { Cmd_UpdateTimer(session, req, root); });
			RegisterCommandCode("deletetimer", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteTimer(session, req, root); });
			RegisterCommandCode("enabletimer", [this](auto&& session, auto&& req, auto&& root) { Cmd_EnableTimer(session, req, root); });
			RegisterCommandCode("disabletimer", [this](auto&& session, auto&& req, auto&& root) { Cmd_DisableTimer(session, req, root); });
			RegisterCommandCode("cleartimers", [this](auto&& session, auto&& req, auto&& root) { Cmd_ClearTimers(session, req, root); });

			RegisterCommandCode("addscenetimer", [this](auto&& session, auto&& req, auto&& root) { Cmd_AddSceneTimer(session, req, root); });
			RegisterCommandCode("updatescenetimer", [this](auto&& session, auto&& req, auto&& root) { Cmd_UpdateSceneTimer(session, req, root); });
			RegisterCommandCode("deletescenetimer", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteSceneTimer(session, req, root); });
			RegisterCommandCode("enablescenetimer", [this](auto&& session, auto&& req, auto&& root) { Cmd_EnableSceneTimer(session, req, root); });
			RegisterCommandCode("disablescenetimer", [this](auto&& session, auto&& req, auto&& root) { Cmd_DisableSceneTimer(session, req, root); });
			RegisterCommandCode("clearscenetimers", [this](auto&& session, auto&& req, auto&& root) { Cmd_ClearSceneTimers(session, req, root); });
			RegisterCommandCode("getsceneactivations", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetSceneActivations(session, req, root); });
			RegisterCommandCode("addscenecode", [this](auto&& session, auto&& req, auto&& root) { Cmd_AddSceneCode(session, req, root); });
			RegisterCommandCode("removescenecode", [this](auto&& session, auto&& req, auto&& root) { Cmd_RemoveSceneCode(session, req, root); });
			RegisterCommandCode("clearscenecodes", [this](auto&& session, auto&& req, auto&& root) { Cmd_ClearSceneCodes(session, req, root); });
			RegisterCommandCode("renamescene", [this](auto&& session, auto&& req, auto&& root) { Cmd_RenameScene(session, req, root); });

			RegisterCommandCode("setsetpoint", [this](auto&& session, auto&& req, auto&& root) { Cmd_SetSetpoint(session, req, root); });
			RegisterCommandCode("addsetpointtimer", [this](auto&& session, auto&& req, auto&& root) { Cmd_AddSetpointTimer(session, req, root); });
			RegisterCommandCode("updatesetpointtimer", [this](auto&& session, auto&& req, auto&& root) { Cmd_UpdateSetpointTimer(session, req, root); });
			RegisterCommandCode("deletesetpointtimer", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteSetpointTimer(session, req, root); });
			RegisterCommandCode("enablesetpointtimer", [this](auto&& session, auto&& req, auto&& root) { Cmd_EnableSetpointTimer(session, req, root); });
			RegisterCommandCode("disablesetpointtimer", [this](auto&& session, auto&& req, auto&& root) { Cmd_DisableSetpointTimer(session, req, root); });
			RegisterCommandCode("clearsetpointtimers", [this](auto&& session, auto&& req, auto&& root) { Cmd_ClearSetpointTimers(session, req, root); });

			RegisterCommandCode("serial_devices", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetSerialDevices(session, req, root); });
			RegisterCommandCode("devices_list", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetDevicesList(session, req, root); });
			RegisterCommandCode("devices_list_onoff", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetDevicesListOnOff(session, req, root); });

			RegisterCommandCode("registerhue", [this](auto&& session, auto&& req, auto&& root) { Cmd_PhilipsHueRegister(session, req, root); });

			RegisterCommandCode("getcustomiconset", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetCustomIconSet(session, req, root); });
			RegisterCommandCode("uploadcustomicon", [this](auto&& session, auto&& req, auto&& root) { Cmd_UploadCustomIcon(session, req, root); });
			RegisterCommandCode("deletecustomicon", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteCustomIcon(session, req, root); });
			RegisterCommandCode("updatecustomicon", [this](auto&& session, auto&& req, auto&& root) { Cmd_UpdateCustomIcon(session, req, root); });

			RegisterCommandCode("renamedevice", [this](auto&& session, auto&& req, auto&& root) { Cmd_RenameDevice(session, req, root); });
			RegisterCommandCode("setdevused", [this](auto&& session, auto&& req, auto&& root) { Cmd_SetDeviceUsed(session, req, root); });

			RegisterCommandCode("addlogmessage", [this](auto&& session, auto&& req, auto&& root) { Cmd_AddLogMessage(session, req, root); });
			RegisterCommandCode("clearshortlog", [this](auto&& session, auto&& req, auto&& root) { Cmd_ClearShortLog(session, req, root); });
			RegisterCommandCode("vacuumdatabase", [this](auto&& session, auto&& req, auto&& root) { Cmd_VacuumDatabase(session, req, root); });

			RegisterCommandCode("addmobiledevice", [this](auto&& session, auto&& req, auto&& root) { Cmd_AddMobileDevice(session, req, root); });
			RegisterCommandCode("updatemobiledevice", [this](auto&& session, auto&& req, auto&& root) { Cmd_UpdateMobileDevice(session, req, root); });
			RegisterCommandCode("deletemobiledevice", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteMobileDevice(session, req, root); });

			RegisterCommandCode("addyeelight", [this](auto&& session, auto&& req, auto&& root) { Cmd_AddYeeLight(session, req, root); });

			RegisterCommandCode("addArilux", [this](auto&& session, auto&& req, auto&& root) { Cmd_AddArilux(session, req, root); });

			RegisterCommandCode("tellstickApplySettings", [this](auto&& session, auto&& req, auto&& root) { Cmd_TellstickApplySettings(session, req, root); });

			// Migrated RTypes to regular commands
			RegisterCommandCode("getusers", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetUsers(session, req, root); });
			RegisterCommandCode("getsettings", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetSettings(session, req, root); });
			RegisterCommandCode("getdevices", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetDevices(session, req, root); });
			RegisterCommandCode("gethardware", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetHardware(session, req, root); });
			RegisterCommandCode("events", [this](auto&& session, auto&& req, auto&& root) { Cmd_Events(session, req, root); });
			RegisterCommandCode("getnotifications", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetNotifications(session, req, root); });
			RegisterCommandCode("createvirtualsensor", [this](auto&& session, auto&& req, auto&& root) { Cmd_CreateMappedSensor(session, req, root); });
			RegisterCommandCode("createdevice", [this](auto&& session, auto&& req, auto&& root) { Cmd_CreateDevice(session, req, root); });

			RegisterCommandCode("getscenelog", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetSceneLog(session, req, root); });
			RegisterCommandCode("getscenes", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetScenes(session, req, root); });
			RegisterCommandCode("addscene", [this](auto&& session, auto&& req, auto&& root) { Cmd_AddScene(session, req, root); });
			RegisterCommandCode("deletescene", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteScene(session, req, root); });
			RegisterCommandCode("updatescene", [this](auto&& session, auto&& req, auto&& root) { Cmd_UpdateScene(session, req, root); });
			RegisterCommandCode("getmobiles", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetMobiles(session, req, root); });
			RegisterCommandCode("getcameras", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetCameras(session, req, root); });
			RegisterCommandCode("getcameras_user", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetCamerasUser(session, req, root); });
			RegisterCommandCode("getschedules", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetSchedules(session, req, root); });
			RegisterCommandCode("gettimers", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetTimers(session, req, root); });
			RegisterCommandCode("getscenetimers", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetSceneTimers(session, req, root); });
			RegisterCommandCode("getsetpointtimers", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetSetpointTimers(session, req, root); });
			RegisterCommandCode("getplans", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetPlans(session, req, root); });
			RegisterCommandCode("getfloorplans", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetFloorPlans(session, req, root); });
			RegisterCommandCode("getlightlog", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetLightLog(session, req, root); });
			RegisterCommandCode("gettextlog", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetTextLog(session, req, root); });
			RegisterCommandCode("gettransfers", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetTransfers(session, req, root); });
			RegisterCommandCode("dotransferdevice", [this](auto&& session, auto&& req, auto&& root) { Cmd_DoTransferDevice(session, req, root); });
			RegisterCommandCode("createrflinkdevice", [this](auto&& session, auto&& req, auto&& root) { Cmd_CreateRFLinkDevice(session, req, root); });
			RegisterCommandCode("createevohomesensor", [this](auto&& session, auto&& req, auto&& root) { Cmd_CreateEvohomeSensor(session, req, root); });
			RegisterCommandCode("bindevohome", [this](auto&& session, auto&& req, auto&& root) { Cmd_BindEvohome(session, req, root); });
			RegisterCommandCode("custom_light_icons", [this](auto&& session, auto&& req, auto&& root) { Cmd_CustomLightIcons(session, req, root); });
			RegisterCommandCode("deletedevice", [this](auto&& session, auto&& req, auto&& root) { Cmd_DeleteDevice(session, req, root); });
			RegisterCommandCode("getshareduserdevices", [this](auto&& session, auto&& req, auto&& root) { Cmd_GetSharedUserDevices(session, req, root); });
			RegisterCommandCode("setshareduserdevices", [this](auto&& session, auto&& req, auto&& root) { Cmd_SetSharedUserDevices(session, req, root); });
			RegisterCommandCode("graph", [this](auto&& session, auto&& req, auto&& root) { Cmd_HandleGraph(session, req, root); });
			RegisterCommandCode("rclientslog", [this](auto&& session, auto&& req, auto&& root) { Cmd_RemoteWebClientsLog(session, req, root); });
			RegisterCommandCode("setused", [this](auto&& session, auto&& req, auto&& root) { Cmd_SetUsed(session, req, root); });

			// Migrated ActionCodes to regular commands
			RegisterCommandCode("setccmetertype", [this](auto&& session, auto&& req, auto&& root) { Cmd_SetCurrentCostUSBType(session, req, root); });

			RegisterCommandCode("clearuserdevices", [this](auto&& session, auto&& req, auto&& root) { Cmd_ClearUserDevices(session, req, root); });

			//MQTT-AD
			RegisterCommandCode("mqttadgetconfig", [this](auto&& session, auto&& req, auto&& root) { Cmd_MQTTAD_GetConfig(session, req, root); });
			RegisterCommandCode("mqttupdatenumber", [this](auto&& session, auto&& req, auto&& root) { Cmd_MQTTAD_UpdateNumber(session, req, root); });

			// EnOcean helpers cmds
			RegisterCommandCode("enoceangetmanufacturers", [this](auto&& session, auto&& req, auto&& root) { Cmd_EnOceanGetManufacturers(session, req, root); });
			RegisterCommandCode("enoceangetrorgs", [this](auto&& session, auto&& req, auto&& root) { Cmd_EnOceanGetRORGs(session, req, root); });
			RegisterCommandCode("enoceangetprofiles", [this](auto&& session, auto&& req, auto&& root) { Cmd_EnOceanGetProfiles(session, req, root); });

			// EnOcean ESP3 cmds
			RegisterCommandCode("esp3enablelearnmode", [this](auto&& session, auto&& req, auto&& root) { Cmd_EnOceanESP3EnableLearnMode(session, req, root); });
			RegisterCommandCode("esp3isnodeteachedin", [this](auto&& session, auto&& req, auto&& root) { Cmd_EnOceanESP3IsNodeTeachedIn(session, req, root); });
			RegisterCommandCode("esp3cancelteachin", [this](auto&& session, auto&& req, auto&& root) { Cmd_EnOceanESP3CancelTeachIn(session, req, root); });

			RegisterCommandCode("esp3controllerreset", [this](auto&& session, auto&& req, auto&& root) { Cmd_EnOceanESP3ControllerReset(session, req, root); });

			RegisterCommandCode("esp3updatenode", [this](auto&& session, auto&& req, auto&& root) { Cmd_EnOceanESP3UpdateNode(session, req, root); });
			RegisterCommandCode("esp3deletenode", [this](auto&& session, auto&& req, auto&& root) { Cmd_EnOceanESP3DeleteNode(session, req, root); });
			RegisterCommandCode("esp3getnodes", [this](auto&& session, auto&& req, auto&& root) { Cmd_EnOceanESP3GetNodes(session, req, root); });

			//Whitelist
			m_pWebEm->RegisterWhitelistURLString("/images/floorplans/plan");

			_log.Debug(DEBUG_WEBSERVER, "WebServer(%s) started with %d Registered Commands", m_server_alias.c_str(), (int)m_webcommands.size());
			m_pWebEm->DebugRegistrations();

			// Start normal worker thread
			m_bDoStop = false;
			m_thread = std::make_shared<std::thread>([this] { Do_Work(); });
			std::string server_name = "WebServer_" + settings.listening_port;
			SetThreadName(m_thread->native_handle(), server_name.c_str());
			return (m_thread != nullptr);
		}

		void CWebServer::StopServer()
		{
			m_bDoStop = true;
			try
			{
				if (m_pWebEm == nullptr)
					return;
				m_pWebEm->Stop();
				if (m_thread)
				{
					m_thread->join();
					m_thread.reset();
				}
				delete m_pWebEm;
				m_pWebEm = nullptr;
			}
			catch (...)
			{
			}
		}

		void CWebServer::SetWebCompressionMode(const _eWebCompressionMode gzmode)
		{
			if (m_pWebEm == nullptr)
				return;
			m_pWebEm->SetWebCompressionMode(gzmode);
		}

		void CWebServer::SetAllowPlainBasicAuth(const bool allow)
		{
			if (m_pWebEm == nullptr)
				return;
			m_pWebEm->SetAllowPlainBasicAuth(allow);
		}

		void CWebServer::SetWebTheme(const std::string& themename)
		{
			if (m_pWebEm == nullptr)
				return;
			m_pWebEm->SetWebTheme(themename);
		}

		void CWebServer::SetWebRoot(const std::string& webRoot)
		{
			if (m_pWebEm == nullptr)
				return;
			m_pWebEm->SetWebRoot(webRoot);
		}

		void CWebServer::SetIamSettings(const iamserver::iam_settings& iamsettings)
		{
			m_iamsettings = iamsettings;
		}

		void CWebServer::RegisterCommandCode(const char* idname, const webserver_response_function& ResponseFunction, bool bypassAuthentication)
		{
			if (m_webcommands.find(idname) != m_webcommands.end())
			{
				_log.Debug(DEBUG_WEBSERVER, "CWebServer::RegisterCommandCode :%s already registered", idname);
				return;
			}
			m_webcommands.insert(std::pair<std::string, webserver_response_function>(std::string(idname), ResponseFunction));
			if (bypassAuthentication)
			{
				m_pWebEm->RegisterWhitelistCommandsString(idname);
			}
		}

		void CWebServer::GetJSonPage(WebEmSession& session, const request& req, reply& rep)
		{
			Json::Value root;
			root["status"] = "ERR";

			std::string rtype = request::findValue(&req, "type");
			if (rtype == "command")
			{
				std::string cparam = request::findValue(&req, "param");
				if (!cparam.empty())
				{
					_log.Debug(DEBUG_WEBSERVER, "CWebServer::GetJSonPage :%s :%s ", cparam.c_str(), req.uri.c_str());

					auto pf = m_webcommands.find(cparam);
					if (pf != m_webcommands.end())
					{
						pf->second(session, req, root);
					}
					else
					{	// See if we still have a Param based version not converted to a proper command
						// TODO: remove this once all param based code has been converted to proper commands
						if (!HandleCommandParam(cparam, session, req, root))
						{
							_log.Debug(DEBUG_WEBSERVER, "CWebServer::GetJSonPage(param)(%s) returned an error!", cparam.c_str());
						}
					}
				}
			} //(rtype=="command")
			else
			{	// TODO: remove this after next stable
				// Could be a call to an old style RType, try to handle it and alert the user to update
				_log.Debug(DEBUG_WEBSERVER, "CWebServer::GetJSonPage(rtype) :%s :%s ", rtype.c_str(), req.uri.c_str());

				std::string altrtype;
				if (rtype.compare("settings") == 0)
				{
					altrtype = "getsettings";
				}
				else if (rtype.compare("users") == 0)
				{
					altrtype = "getusers";
				}
				else if (rtype.compare("devices") == 0)
				{
					altrtype = "getdevices";
				}
				else if (rtype.compare("hardware") == 0)
				{
					altrtype = "gethardware";
				}
				else if (rtype.compare("scenes") == 0)
				{
					altrtype = "getscenes";
				}
				else if (rtype.compare("notifications") == 0)
				{
					altrtype = "getnotifications";
				}
				else if (rtype.compare("scenelog") == 0)
				{
					altrtype = "getscenelog";
				}
				else if (rtype.compare("mobiles") == 0)
				{
					altrtype = "getmobiles";
				}
				else if (rtype.compare("cameras") == 0)
				{
					altrtype = "getcameras";
				}
				else if (rtype.compare("cameras_user") == 0)
				{
					altrtype = "getcameras_user";
				}
				else if (rtype.compare("schedules") == 0)
				{
					altrtype = "getschedules";
				}
				else if (rtype.compare("timers") == 0)
				{
					altrtype = "gettimers";
				}
				else if (rtype.compare("scenetimers") == 0)
				{
					altrtype = "getscenetimers";
				}
				else if (rtype.compare("setpointtimers") == 0)
				{
					altrtype = "getsetpointtimers";
				}
				else if (rtype.compare("plans") == 0)
				{
					altrtype = "getplans";
				}
				else if (rtype.compare("floorplans") == 0)
				{
					altrtype = "getfloorplans";
				}
				else if (rtype.compare("lightlog") == 0)
				{
					altrtype = "getlightlog";
				}
				else if (rtype.compare("textlog") == 0)
				{
					altrtype = "gettextlog";
				}
				else if (rtype.compare("graph") == 0)
				{
					altrtype = "graph";
				}
				else if (rtype.compare("createdevice") == 0)
				{
					altrtype = "createdevice";
				}
				else if (rtype.compare("setused") == 0)
				{
					altrtype = "setused";
				}

				if (!altrtype.empty())
				{
					auto pf = m_webcommands.find(altrtype);
					if (pf != m_webcommands.end())
					{
						_log.Log(LOG_NORM, "[WebServer] Deprecated RType (%s) for API request. Handled via fallback (%s), please use correct API Command! (%s)", rtype.c_str(), altrtype.c_str(), req.host_remote_address.c_str());
						pf->second(session, req, root);
					}
				}
				else
				{
					_log.Log(LOG_STATUS, "[WebServer] Deprecated RType (%s) for API request. Call ignored, please use correct API Command! (%s)", rtype.c_str(), req.host_remote_address.c_str());
				}

			}

			reply::set_content(&rep, root.toStyledString());
		}

		void CWebServer::Cmd_GetTimerTypes(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["title"] = "GetTimerTypes";
			for (int ii = 0; ii < TTYPE_END; ii++)
			{
				std::string sTimerTypeDesc = Timer_Type_Desc(_eTimerType(ii));
				root["result"][ii] = sTimerTypeDesc;
			}
			root["status"] = "OK";
		}

		void CWebServer::Cmd_GetLanguages(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["title"] = "GetLanguages";
			std::string sValue;
			if (m_sql.GetPreferencesVar("Language", sValue))
			{
				root["language"] = sValue;
			}
			for (auto& lang : guiLanguage)
			{
				root["result"][lang.second] = lang.first;
			}
			root["status"] = "OK";
		}

		void CWebServer::Cmd_GetSwitchTypes(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["title"] = "GetSwitchTypes";

			std::map<std::string, int> _switchtypes;

			for (int ii = 0; ii < STYPE_END; ii++)
			{
				std::string sTypeName = Switch_Type_Desc((_eSwitchType)ii);
				if (sTypeName != "Unknown")
				{
					_switchtypes[sTypeName] = ii;
				}
			}
			// return a sorted list
			for (const auto& type : _switchtypes)
			{
				root["result"][type.second] = type.first;
			}
			root["status"] = "OK";
		}

		void CWebServer::Cmd_GetMeterTypes(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["title"] = "GetMeterTypes";

			for (int ii = 0; ii < MTYPE_END; ii++)
			{
				std::string sTypeName = Meter_Type_Desc((_eMeterType)ii);
				root["result"][ii] = sTypeName;
			}
			root["status"] = "OK";
		}

		void CWebServer::Cmd_GetThemes(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "OK";
			root["title"] = "GetThemes";
			m_mainworker.GetAvailableWebThemes();
			int ii = 0;
			for (const auto& theme : m_mainworker.m_webthemes)
			{
				root["result"][ii]["theme"] = theme;
				ii++;
			}
		}

		void CWebServer::Cmd_GetTitle(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string sValue;
			root["status"] = "OK";
			root["title"] = "GetTitle";
			if (m_sql.GetPreferencesVar("Title", sValue))
				root["Title"] = sValue;
			else
				root["Title"] = "Domoticz";
		}

		void CWebServer::Cmd_LoginCheck(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string tmpusrname = request::findValue(&req, "username");
			std::string tmpusrpass = request::findValue(&req, "password");
			if ((tmpusrname.empty()) || (tmpusrpass.empty()))
				return;

			std::string rememberme = request::findValue(&req, "rememberme");

			std::string usrname;
			std::string usrpass;
			if (request_handler::url_decode(tmpusrname, usrname))
			{
				if (request_handler::url_decode(tmpusrpass, usrpass))
				{
					usrname = base64_decode(usrname);
					int iUser = FindUser(usrname.c_str());
					if (iUser == -1)
					{
						// log brute force attack
						_log.Log(LOG_ERROR, "Failed login attempt from %s for user '%s' !", session.remote_host.c_str(), usrname.c_str());
						return;
					}
					if (m_users[iUser].Password != usrpass)
					{
						// log brute force attack
						_log.Log(LOG_ERROR, "Failed login attempt from %s for '%s' !", session.remote_host.c_str(), m_users[iUser].Username.c_str());
						return;
					}
					if (m_users[iUser].userrights == URIGHTS_CLIENTID) {
						// Not a right for users to login with
						_log.Log(LOG_ERROR, "Failed login attempt from %s for '%s' !", session.remote_host.c_str(), m_users[iUser].Username.c_str());
						return;
					}
					if (!m_users[iUser].Mfatoken.empty())
					{
						// 2FA enabled for this user
						std::string tmp2fa = request::findValue(&req, "2fatotp");
						std::string sTotpKey = "";
						if(!base32_decode(m_users[iUser].Mfatoken, sTotpKey))
						{
							// Unable to decode the 2FA token
							_log.Log(LOG_ERROR, "Failed login attempt from %s for '%s' !", session.remote_host.c_str(), m_users[iUser].Username.c_str());
							_log.Debug(DEBUG_AUTH, "Failed to base32_decode the Users 2FA token: %s", m_users[iUser].Mfatoken.c_str());
							return;
						}
						if (tmp2fa.empty())
						{
							// No 2FA token given (yet), request one
							root["status"] = "OK";
							root["title"] = "logincheck";
							root["require2fa"] = "true";
							return;
						}
						if (!VerifySHA1TOTP(tmp2fa, sTotpKey))
						{
							// Not a match for the given 2FA token
							_log.Log(LOG_ERROR, "Failed login attempt from %s for '%s' !", session.remote_host.c_str(), m_users[iUser].Username.c_str());
							_log.Debug(DEBUG_AUTH, "Failed login attempt with 2FA token: %s", tmp2fa.c_str());
							return;
						}
					}
					_log.Log(LOG_STATUS, "Login successful from %s for user '%s'", session.remote_host.c_str(), m_users[iUser].Username.c_str());
					root["status"] = "OK";
					root["version"] = szAppVersion;
					root["title"] = "logincheck";
					session.isnew = true;
					session.username = m_users[iUser].Username;
					session.rights = m_users[iUser].userrights;
					session.rememberme = (rememberme == "true");
					root["user"] = session.username;
					root["rights"] = session.rights;
				}
			}
		}

		void CWebServer::Cmd_GetHardwareTypes(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			root["status"] = "OK";
			root["title"] = "GetHardwareTypes";
			std::map<std::string, int> _htypes;
			for (int ii = 0; ii < HTYPE_END; ii++)
			{
				bool bDoAdd = true;
#ifndef _DEBUG
#ifdef WIN32
				if ((ii == HTYPE_RaspberryBMP085) || (ii == HTYPE_RaspberryHTU21D) || (ii == HTYPE_RaspberryTSL2561) || (ii == HTYPE_RaspberryPCF8574) ||
					(ii == HTYPE_RaspberryBME280) || (ii == HTYPE_RaspberryMCP23017))
				{
					bDoAdd = false;
				}
				else
				{
#ifndef WITH_LIBUSB
					if ((ii == HTYPE_VOLCRAFTCO20) || (ii == HTYPE_TE923))
					{
						bDoAdd = false;
					}
#endif
				}
#endif
#endif
#ifndef WITH_GPIO
				if (ii == HTYPE_RaspberryGPIO)
				{
					bDoAdd = false;
				}

				if (ii == HTYPE_SysfsGpio)
				{
					bDoAdd = false;
				}
#endif
				if (ii == HTYPE_PythonPlugin)
					bDoAdd = false;

				if (bDoAdd)
				{
					std::string description = Hardware_Type_Desc(ii);
					if (!description.empty())
						_htypes[description] = ii;
				}
			}

			// return a sorted hardware list
			int ii = 0;
			for (const auto& type : _htypes)
			{
				root["result"][ii]["idx"] = type.second;
				root["result"][ii]["name"] = type.first;
				ii++;
			}

#ifdef ENABLE_PYTHON
			// Append Plugin list as well
			PluginList(root["result"]);
#endif
		}

		void CWebServer::Cmd_AddHardware(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string name = HTMLSanitizer::Sanitize(CURLEncode::URLDecode(request::findValue(&req, "name")));
			std::string senabled = request::findValue(&req, "enabled");
			std::string shtype = request::findValue(&req, "htype");
			std::string loglevel = request::findValue(&req, "loglevel");
			std::string address = HTMLSanitizer::Sanitize(request::findValue(&req, "address"));
			std::string sport = request::findValue(&req, "port");
			std::string username = HTMLSanitizer::Sanitize(request::findValue(&req, "username"));
			std::string password = request::findValue(&req, "password");
			std::string extra = CURLEncode::URLDecode(request::findValue(&req, "extra"));
			std::string sdatatimeout = request::findValue(&req, "datatimeout");
			if ((name.empty()) || (senabled.empty()) || (shtype.empty()))
				return;
			_eHardwareTypes htype = (_eHardwareTypes)atoi(shtype.c_str());

			int iDataTimeout = atoi(sdatatimeout.c_str());
			int mode1 = 0;
			int mode2 = 0;
			int mode3 = 0;
			int mode4 = 0;
			int mode5 = 0;
			int mode6 = 0;
			int port = atoi(sport.c_str());
			uint32_t iLogLevelEnabled = (uint32_t)atoi(loglevel.c_str());
			std::string mode1Str = request::findValue(&req, "Mode1");
			if (!mode1Str.empty())
			{
				mode1 = atoi(mode1Str.c_str());
			}
			std::string mode2Str = request::findValue(&req, "Mode2");
			if (!mode2Str.empty())
			{
				mode2 = atoi(mode2Str.c_str());
			}
			std::string mode3Str = request::findValue(&req, "Mode3");
			if (!mode3Str.empty())
			{
				mode3 = atoi(mode3Str.c_str());
			}
			std::string mode4Str = request::findValue(&req, "Mode4");
			if (!mode4Str.empty())
			{
				mode4 = atoi(mode4Str.c_str());
			}
			std::string mode5Str = request::findValue(&req, "Mode5");
			if (!mode5Str.empty())
			{
				mode5 = atoi(mode5Str.c_str());
			}
			std::string mode6Str = request::findValue(&req, "Mode6");
			if (!mode6Str.empty())
			{
				mode6 = atoi(mode6Str.c_str());
			}

			if (IsSerialDevice(htype))
			{
				// USB/System
				if (sport.empty())
					return; // need to have a serial port

				if (htype == HTYPE_TeleinfoMeter)
				{
					// Teleinfo always has decimals. Chances to have a P1 and a Teleinfo device on the same
					// Domoticz instance are very low as both are national standards (NL and FR)
					m_sql.UpdatePreferencesVar("SmartMeterType", 0);
				}
			}
			else if (IsNetworkDevice(htype))
			{
				// Lan
				if (address.empty())
					return;

				if (htype == HTYPE_MySensorsMQTT || htype == HTYPE_MQTT || htype == HTYPE_MQTTAutoDiscovery)
				{
					std::string modeqStr = request::findValue(&req, "mode1");
					if (!modeqStr.empty())
					{
						mode1 = atoi(modeqStr.c_str());
					}
				}
				else if (htype == HTYPE_ECODEVICES || htype == HTYPE_TeleinfoMeterTCP)
				{
					// EcoDevices and Teleinfo always have decimals. Chances to have a P1 and a EcoDevice/Teleinfo
					//  device on the same Domoticz instance are very low as both are national standards (NL and FR)
					m_sql.UpdatePreferencesVar("SmartMeterType", 0);
				}
				else if (htype == HTYPE_AlfenEveCharger)
				{
					if ((password.empty()))
						return;
				}
			}
			else if (htype == HTYPE_DomoticzInternal)
			{
				// DomoticzInternal cannot be added manually
				return;
			}
			else if (htype == HTYPE_Domoticz)
			{
				// Remote Domoticz
				if (address.empty() || port == 0)
					return;
			}
			else if (htype == HTYPE_TE923)
			{
				// all fine here!
			}
			else if (htype == HTYPE_VOLCRAFTCO20)
			{
				// all fine here!
			}
			else if (htype == HTYPE_System)
			{
				// There should be only one
				std::vector<std::vector<std::string>> result;
				result = m_sql.safe_query("SELECT ID FROM Hardware WHERE (Type==%d)", HTYPE_System);
				if (!result.empty())
					return;
			}
			else if (htype == HTYPE_1WIRE)
			{
				// all fine here!
			}
			else if (htype == HTYPE_Rtl433)
			{
				// all fine here!
			}
			else if (htype == HTYPE_Pinger)
			{
				// all fine here!
			}
			else if (htype == HTYPE_Kodi)
			{
				// all fine here!
			}
			else if (htype == HTYPE_PanasonicTV)
			{
				// all fine here!
			}
			else if (htype == HTYPE_LogitechMediaServer)
			{
				// all fine here!
			}
			else if (htype == HTYPE_RaspberryBMP085)
			{
				// all fine here!
			}
			else if (htype == HTYPE_RaspberryHTU21D)
			{
				// all fine here!
			}
			else if (htype == HTYPE_RaspberryTSL2561)
			{
				// all fine here!
			}
			else if (htype == HTYPE_RaspberryBME280)
			{
				// all fine here!
			}
			else if (htype == HTYPE_RaspberryMCP23017)
			{
				// all fine here!
			}
			else if (htype == HTYPE_Dummy)
			{
				// all fine here!
			}
			else if (htype == HTYPE_Tellstick)
			{
				// all fine here!
			}
			else if (htype == HTYPE_EVOHOME_SCRIPT || htype == HTYPE_EVOHOME_SERIAL || htype == HTYPE_EVOHOME_WEB || htype == HTYPE_EVOHOME_TCP)
			{
				// all fine here!
			}
			else if (htype == HTYPE_PiFace)
			{
				// all fine here!
			}
			else if (htype == HTYPE_HTTPPOLLER)
			{
				// all fine here!
			}
			else if (htype == HTYPE_BleBox)
			{
				// all fine here!
			}
			else if (htype == HTYPE_HEOS)
			{
				// all fine here!
			}
			else if (htype == HTYPE_Yeelight)
			{
				// all fine here!
			}
			else if (htype == HTYPE_XiaomiGateway)
			{
				// all fine here!
			}
			else if (htype == HTYPE_Arilux)
			{
				// all fine here!
			}
			else if (htype == HTYPE_USBtinGateway)
			{
				// All fine here
			}
			else if (htype == HTYPE_BuienRadar)
			{
				// All fine here
			}
			else if ((htype == HTYPE_Wunderground) || (htype == HTYPE_DarkSky) || (htype == HTYPE_VisualCrossing) || (htype == HTYPE_AccuWeather) || (htype == HTYPE_OpenWeatherMap) || (htype == HTYPE_ICYTHERMOSTAT) ||
				(htype == HTYPE_TOONTHERMOSTAT) || (htype == HTYPE_AtagOne) || (htype == HTYPE_PVOUTPUT_INPUT) || (htype == HTYPE_NEST) || (htype == HTYPE_ANNATHERMOSTAT) ||
				(htype == HTYPE_THERMOSMART) || (htype == HTYPE_Tado) || (htype == HTYPE_Tesla) || (htype == HTYPE_Mercedes) || (htype == HTYPE_Netatmo))
			{
				if ((username.empty()) || (password.empty()))
					return;
			}
			else if (htype == HTYPE_SolarEdgeAPI)
			{
				if ((username.empty()))
					return;
			}
			else if (htype == HTYPE_Nest_OAuthAPI)
			{
				if ((username.empty()) && (extra == "||"))
					return;
			}
			else if (htype == HTYPE_SBFSpot)
			{
				if (username.empty())
					return;
			}
			else if (htype == HTYPE_HARMONY_HUB)
			{
				if ((address.empty() || port == 0))
					return;
			}
			else if (htype == HTYPE_Philips_Hue)
			{
				if ((username.empty()) || (address.empty() || port == 0))
					return;
				if (port == 0)
					port = 80;
			}
			else if (htype == HTYPE_WINDDELEN)
			{
				std::string mill_id = request::findValue(&req, "Mode1");
				if ((mill_id.empty()) || (sport.empty()))

					return;
				mode1 = atoi(mill_id.c_str());
			}
			else if (htype == HTYPE_Honeywell)
			{
				// all fine here!
			}
			else if (htype == HTYPE_RaspberryGPIO)
			{
				// all fine here!
			}
			else if (htype == HTYPE_SysfsGpio)
			{
				// all fine here!
			}
			else if (htype == HTYPE_OpenWebNetTCP)
			{
				// All fine here
			}
			else if (htype == HTYPE_Daikin)
			{
				// All fine here
			}
			else if (htype == HTYPE_PythonPlugin)
			{
				// All fine here
			}
			else if (htype == HTYPE_RaspberryPCF8574)
			{
				// All fine here
			}
			else if (htype == HTYPE_OpenWebNetUSB)
			{
				// All fine here
			}
			else if (htype == HTYPE_IntergasInComfortLAN2RF)
			{
				// All fine here
			}
			else if (htype == HTYPE_EnphaseAPI)
			{
				// All fine here
			}
			else if (htype == HTYPE_EcoCompteur)
			{
				// all fine here!
			}
			else if (htype == HTYPE_Meteorologisk)
			{
				// all fine here!
			}
			else if (htype == HTYPE_AirconWithMe)
			{
				// all fine here!
			}
			else if (htype == HTYPE_EneverPriceFeeds)
			{
				// All fine here
			}
			else
				return;

			root["status"] = "OK";
			root["title"] = "AddHardware";

			std::vector<std::vector<std::string>> result;

			if (htype == HTYPE_Domoticz)
			{
				if (password.size() != 32)
				{
					password = GenerateMD5Hash(password);
				}
			}
			else if ((htype == HTYPE_S0SmartMeterUSB) || (htype == HTYPE_S0SmartMeterTCP))
			{
				extra = "0;1000;0;1000;0;1000;0;1000;0;1000";
			}
			else if (htype == HTYPE_Pinger)
			{
				mode1 = 30;
				mode2 = 1000;
			}
			else if (htype == HTYPE_Kodi)
			{
				mode1 = 30;
				mode2 = 1000;
			}
			else if (htype == HTYPE_PanasonicTV)
			{
				mode1 = 30;
				mode2 = 1000;
			}
			else if (htype == HTYPE_LogitechMediaServer)
			{
				mode1 = 30;
				mode2 = 1000;
			}
			else if (htype == HTYPE_HEOS)
			{
				mode1 = 30;
				mode2 = 1000;
			}
			else if (htype == HTYPE_Tellstick)
			{
				mode1 = 4;
				mode2 = 500;
			}

			if (htype == HTYPE_HTTPPOLLER)
			{
				m_sql.safe_query("INSERT INTO Hardware (Name, Enabled, Type, LogLevel, Address, Port, SerialPort, Username, Password, Extra, Mode1, Mode2, Mode3, Mode4, Mode5, Mode6, "
					"DataTimeout) VALUES ('%q',%d, %d, %d,'%q',%d,'%q','%q','%q','%q','%q','%q', '%q', '%q', '%q', '%q', %d)",
					name.c_str(), (senabled == "true") ? 1 : 0, htype, iLogLevelEnabled, address.c_str(), port, sport.c_str(), username.c_str(), password.c_str(),
					extra.c_str(), mode1Str.c_str(), mode2Str.c_str(), mode3Str.c_str(), mode4Str.c_str(), mode5Str.c_str(), mode6Str.c_str(), iDataTimeout);
			}
			else if (htype == HTYPE_PythonPlugin)
			{
				sport = request::findValue(&req, "serialport");
				m_sql.safe_query("INSERT INTO Hardware (Name, Enabled, Type, LogLevel, Address, Port, SerialPort, Username, Password, Extra, Mode1, Mode2, Mode3, Mode4, Mode5, Mode6, "
					"DataTimeout) VALUES ('%q',%d, %d, %d,'%q',%d,'%q','%q','%q','%q','%q','%q', '%q', '%q', '%q', '%q', %d)",
					name.c_str(), (senabled == "true") ? 1 : 0, htype, iLogLevelEnabled, address.c_str(), port, sport.c_str(), username.c_str(), password.c_str(),
					extra.c_str(), mode1Str.c_str(), mode2Str.c_str(), mode3Str.c_str(), mode4Str.c_str(), mode5Str.c_str(), mode6Str.c_str(), iDataTimeout);
			}
			else if ((htype == HTYPE_RFXtrx433) || (htype == HTYPE_RFXtrx868))
			{
				// No Extra field here, handled in CWebServer::SetRFXCOMMode
				m_sql.safe_query("INSERT INTO Hardware (Name, Enabled, Type, LogLevel, Address, Port, SerialPort, Username, Password, Mode1, Mode2, Mode3, Mode4, Mode5, Mode6, "
					"DataTimeout) VALUES ('%q',%d, %d, %d,'%q',%d,'%q','%q','%q',%d,%d,%d,%d,%d,%d,%d)",
					name.c_str(), (senabled == "true") ? 1 : 0, htype, iLogLevelEnabled, address.c_str(), port, sport.c_str(), username.c_str(), password.c_str(), mode1,
					mode2, mode3, mode4, mode5, mode6, iDataTimeout);
				extra = "0";
			}
			else
			{
				m_sql.safe_query("INSERT INTO Hardware (Name, Enabled, Type, LogLevel, Address, Port, SerialPort, Username, Password, Extra, Mode1, Mode2, Mode3, Mode4, Mode5, Mode6, "
					"DataTimeout) VALUES ('%q',%d, %d, %d,'%q',%d,'%q','%q','%q','%q',%d,%d,%d,%d,%d,%d,%d)",
					name.c_str(), (senabled == "true") ? 1 : 0, htype, iLogLevelEnabled, address.c_str(), port, sport.c_str(), username.c_str(), password.c_str(),
					extra.c_str(), mode1, mode2, mode3, mode4, mode5, mode6, iDataTimeout);
			}

			// add the device for real in our system
			result = m_sql.safe_query("SELECT MAX(ID) FROM Hardware");
			if (!result.empty())
			{
				std::vector<std::string> sd = result[0];
				int ID = atoi(sd[0].c_str());

				root["idx"] = sd[0].c_str(); // OTO output the created ID for easier management on the caller side (if automated)

				m_mainworker.AddHardwareFromParams(ID, name, (senabled == "true") ? true : false, htype, iLogLevelEnabled, address, port, sport, username, password, extra, mode1,
					mode2, mode3, mode4, mode5, mode6, iDataTimeout, true);
			}
		}

		void CWebServer::Cmd_UpdateHardware(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			std::string name = HTMLSanitizer::Sanitize(CURLEncode::URLDecode(request::findValue(&req, "name")));
			std::string senabled = request::findValue(&req, "enabled");
			std::string shtype = request::findValue(&req, "htype");
			std::string loglevel = request::findValue(&req, "loglevel");
			std::string address = HTMLSanitizer::Sanitize(request::findValue(&req, "address"));
			std::string sport = request::findValue(&req, "port");
			std::string username = HTMLSanitizer::Sanitize(request::findValue(&req, "username"));
			std::string password = request::findValue(&req, "password");
			std::string extra = HTMLSanitizer::Sanitize(CURLEncode::URLDecode(request::findValue(&req, "extra")));
			std::string sdatatimeout = request::findValue(&req, "datatimeout");

			if ((name.empty()) || (senabled.empty()) || (shtype.empty()))
				return;

			int mode1 = atoi(request::findValue(&req, "Mode1").c_str());
			int mode2 = atoi(request::findValue(&req, "Mode2").c_str());
			int mode3 = atoi(request::findValue(&req, "Mode3").c_str());
			int mode4 = atoi(request::findValue(&req, "Mode4").c_str());
			int mode5 = atoi(request::findValue(&req, "Mode5").c_str());
			int mode6 = atoi(request::findValue(&req, "Mode6").c_str());

			bool bEnabled = (senabled == "true") ? true : false;

			_eHardwareTypes htype = (_eHardwareTypes)atoi(shtype.c_str());
			int iDataTimeout = atoi(sdatatimeout.c_str());

			int port = atoi(sport.c_str());
			uint32_t iLogLevelEnabled = (uint32_t)atoi(loglevel.c_str());

			bool bIsSerial = false;

			if (IsSerialDevice(htype))
			{
				// USB/System
				bIsSerial = true;
				if (bEnabled)
				{
					if (sport.empty())
						return; // need to have a serial port
				}
			}
			else if (IsNetworkDevice(htype))
			{
				// Lan
				if (address.empty())
					return;

				if (htype == HTYPE_MySensorsMQTT || htype == HTYPE_MQTT || htype == HTYPE_MQTTAutoDiscovery)
				{
					std::string modeqStr = request::findValue(&req, "mode1");
					if (!modeqStr.empty())
					{
						mode1 = atoi(modeqStr.c_str());
					}
				}
				else if (htype == HTYPE_ECODEVICES || htype == HTYPE_TeleinfoMeterTCP)
				{
					// EcoDevices and Teleinfo always have decimals. Chances to have a P1 and a EcoDevice/Teleinfo
					//  device on the same Domoticz instance are very low as both are national standards (NL and FR)
					m_sql.UpdatePreferencesVar("SmartMeterType", 0);
				}
				else if (htype == HTYPE_AlfenEveCharger)
				{
					if ((password.empty()))
						return;
				}
			}
			else if (htype == HTYPE_DomoticzInternal)
			{
				// DomoticzInternal cannot be updated
				return;
			}
			else if (htype == HTYPE_Domoticz)
			{
				// Remote Domoticz
				if (address.empty())
					return;
			}
			else if (htype == HTYPE_System)
			{
				// There should be only one, and with this ID
				std::vector<std::vector<std::string>> result;
				result = m_sql.safe_query("SELECT ID FROM Hardware WHERE (Type==%d)", HTYPE_System);
				if (!result.empty())
				{
					int hID = atoi(result[0][0].c_str());
					int aID = atoi(idx.c_str());
					if (hID != aID)
						return;
				}
			}
			else if (htype == HTYPE_TE923)
			{
				// All fine here
			}
			else if (htype == HTYPE_VOLCRAFTCO20)
			{
				// All fine here
			}
			else if (htype == HTYPE_1WIRE)
			{
				// All fine here
			}
			else if (htype == HTYPE_Pinger)
			{
				// All fine here
			}
			else if (htype == HTYPE_Kodi)
			{
				// All fine here
			}
			else if (htype == HTYPE_PanasonicTV)
			{
				// All fine here
			}
			else if (htype == HTYPE_LogitechMediaServer)
			{
				// All fine here
			}
			else if (htype == HTYPE_RaspberryBMP085)
			{
				// All fine here
			}
			else if (htype == HTYPE_RaspberryHTU21D)
			{
				// All fine here
			}
			else if (htype == HTYPE_RaspberryTSL2561)
			{
				// All fine here
			}
			else if (htype == HTYPE_RaspberryBME280)
			{
				// All fine here
			}
			else if (htype == HTYPE_RaspberryMCP23017)
			{
				// all fine here!
			}
			else if (htype == HTYPE_Dummy)
			{
				// All fine here
			}
			else if (htype == HTYPE_EVOHOME_SCRIPT || htype == HTYPE_EVOHOME_SERIAL || htype == HTYPE_EVOHOME_WEB || htype == HTYPE_EVOHOME_TCP)
			{
				// All fine here
			}
			else if (htype == HTYPE_PiFace)
			{
				// All fine here
			}
			else if (htype == HTYPE_HTTPPOLLER)
			{
				// all fine here!
			}
			else if (htype == HTYPE_BleBox)
			{
				// All fine here
			}
			else if (htype == HTYPE_HEOS)
			{
				// All fine here
			}
			else if (htype == HTYPE_Yeelight)
			{
				// All fine here
			}
			else if (htype == HTYPE_XiaomiGateway)
			{
				// All fine here
			}
			else if (htype == HTYPE_Arilux)
			{
				// All fine here
			}
			else if (htype == HTYPE_USBtinGateway)
			{
				// All fine here
			}
			else if (htype == HTYPE_BuienRadar)
			{
				// All fine here
			}
			else if ((htype == HTYPE_Wunderground) || (htype == HTYPE_DarkSky) || (htype == HTYPE_VisualCrossing) || (htype == HTYPE_AccuWeather) || (htype == HTYPE_OpenWeatherMap) || (htype == HTYPE_ICYTHERMOSTAT) ||
				(htype == HTYPE_TOONTHERMOSTAT) || (htype == HTYPE_AtagOne) || (htype == HTYPE_PVOUTPUT_INPUT) || (htype == HTYPE_NEST) || (htype == HTYPE_ANNATHERMOSTAT) ||
				(htype == HTYPE_THERMOSMART) || (htype == HTYPE_Tado) || (htype == HTYPE_Tesla) || (htype == HTYPE_Mercedes) || (htype == HTYPE_Netatmo))
			{
				if ((username.empty()) || (password.empty()))
					return;
			}
			else if (htype == HTYPE_SolarEdgeAPI)
			{
				if ((username.empty()))
					return;
			}
			else if (htype == HTYPE_Nest_OAuthAPI)
			{
				if ((username.empty()) && (extra == "||"))
					return;
			}
			else if (htype == HTYPE_HARMONY_HUB)
			{
				if ((address.empty()))
					return;
			}
			else if (htype == HTYPE_Philips_Hue)
			{
				if ((username.empty()) || (address.empty()))
					return;
				if (port == 0)
					port = 80;
			}
			else if (htype == HTYPE_RaspberryGPIO)
			{
				// all fine here!
			}
			else if (htype == HTYPE_SysfsGpio)
			{
				// all fine here!
			}
			else if (htype == HTYPE_Rtl433)
			{
				// all fine here!
			}
			else if (htype == HTYPE_Daikin)
			{
				// all fine here!
			}
			else if (htype == HTYPE_SBFSpot)
			{
				if (username.empty())
					return;
			}
			else if (htype == HTYPE_WINDDELEN)
			{
				std::string mill_id = request::findValue(&req, "Mode1");
				if ((mill_id.empty()) || (sport.empty()))
					return;
			}
			else if (htype == HTYPE_Honeywell)
			{
				// All fine here
			}
			else if (htype == HTYPE_OpenWebNetTCP)
			{
				// All fine here
			}
			else if (htype == HTYPE_PythonPlugin)
			{
				// All fine here
			}
			else if (htype == HTYPE_RaspberryPCF8574)
			{
				// All fine here
			}
			else if (htype == HTYPE_OpenWebNetUSB)
			{
				// All fine here
			}
			else if (htype == HTYPE_IntergasInComfortLAN2RF)
			{
				// All fine here
			}
			else if (htype == HTYPE_EnphaseAPI)
			{
				// all fine here!
			}
			else if (htype == HTYPE_Meteorologisk)
			{
				// all fine here!
			}
			else if (htype == HTYPE_AirconWithMe)
			{
				// all fine here!
			}
			else if (htype == HTYPE_RFLINKMQTT)
			{
				//all fine here!
			}
			else if (htype == HTYPE_EneverPriceFeeds)
			{
				// all fine here!
			}
			else
				return;

			std::string mode1Str;
			std::string mode2Str;
			std::string mode3Str;
			std::string mode4Str;
			std::string mode5Str;
			std::string mode6Str;

			root["status"] = "OK";
			root["title"] = "UpdateHardware";

			if (htype == HTYPE_Domoticz)
			{
				if (password.size() != 32)
				{
					password = GenerateMD5Hash(password);
				}
			}

			if ((bIsSerial) && (!bEnabled) && (sport.empty()))
			{
				// just disable the device
				m_sql.safe_query("UPDATE Hardware SET Enabled=%d WHERE (ID == '%q')", (bEnabled == true) ? 1 : 0, idx.c_str());
			}
			else
			{
				if (htype == HTYPE_HTTPPOLLER)
				{
					m_sql.safe_query("UPDATE Hardware SET Name='%q', Enabled=%d, Type=%d, LogLevel=%d, Address='%q', Port=%d, SerialPort='%q', Username='%q', Password='%q', "
						"Extra='%q', DataTimeout=%d WHERE (ID == '%q')",
						name.c_str(), (senabled == "true") ? 1 : 0, htype, iLogLevelEnabled, address.c_str(), port, sport.c_str(), username.c_str(), password.c_str(),
						extra.c_str(), iDataTimeout, idx.c_str());
				}
				else if (htype == HTYPE_PythonPlugin)
				{
					mode1Str = request::findValue(&req, "Mode1");
					mode2Str = request::findValue(&req, "Mode2");
					mode3Str = request::findValue(&req, "Mode3");
					mode4Str = request::findValue(&req, "Mode4");
					mode5Str = request::findValue(&req, "Mode5");
					mode6Str = request::findValue(&req, "Mode6");
					sport = request::findValue(&req, "serialport");
					m_sql.safe_query("UPDATE Hardware SET Name='%q', Enabled=%d, Type=%d, LogLevel=%d, Address='%q', Port=%d, SerialPort='%q', Username='%q', Password='%q', "
						"Extra='%q', Mode1='%q', Mode2='%q', Mode3='%q', Mode4='%q', Mode5='%q', Mode6='%q', DataTimeout=%d WHERE (ID == '%q')",
						name.c_str(), (senabled == "true") ? 1 : 0, htype, iLogLevelEnabled, address.c_str(), port, sport.c_str(), username.c_str(), password.c_str(),
						extra.c_str(), mode1Str.c_str(), mode2Str.c_str(), mode3Str.c_str(), mode4Str.c_str(), mode5Str.c_str(), mode6Str.c_str(), iDataTimeout,
						idx.c_str());
				}
				else if ((htype == HTYPE_RFXtrx433) || (htype == HTYPE_RFXtrx868))
				{
					// No Extra field here, handled in CWebServer::SetRFXCOMMode
					m_sql.safe_query("UPDATE Hardware SET Name='%q', Enabled=%d, Type=%d, LogLevel=%d, Address='%q', Port=%d, SerialPort='%q', Username='%q', Password='%q', "
						"Mode1=%d, Mode2=%d, Mode3=%d, Mode4=%d, Mode5=%d, Mode6=%d, DataTimeout=%d WHERE (ID == '%q')",
						name.c_str(), (bEnabled == true) ? 1 : 0, htype, iLogLevelEnabled, address.c_str(), port, sport.c_str(), username.c_str(), password.c_str(),
						mode1, mode2, mode3, mode4, mode5, mode6, iDataTimeout, idx.c_str());
					std::vector<std::vector<std::string>> result;
					result = m_sql.safe_query("SELECT Extra FROM Hardware WHERE ID=%q", idx.c_str());
					if (!result.empty())
						extra = result[0][0];
				}
				else
				{
					m_sql.safe_query("UPDATE Hardware SET Name='%q', Enabled=%d, Type=%d, LogLevel=%d, Address='%q', Port=%d, SerialPort='%q', Username='%q', Password='%q', "
						"Extra='%q', Mode1=%d, Mode2=%d, Mode3=%d, Mode4=%d, Mode5=%d, Mode6=%d, DataTimeout=%d WHERE (ID == '%q')",
						name.c_str(), (bEnabled == true) ? 1 : 0, htype, iLogLevelEnabled, address.c_str(), port, sport.c_str(), username.c_str(), password.c_str(),
						extra.c_str(), mode1, mode2, mode3, mode4, mode5, mode6, iDataTimeout, idx.c_str());
				}
			}

			// re-add the device in our system
			int ID = atoi(idx.c_str());
			m_mainworker.AddHardwareFromParams(ID, name, bEnabled, htype, iLogLevelEnabled, address, port, sport, username, password, extra, mode1, mode2, mode3, mode4, mode5, mode6,
				iDataTimeout, true);
		}

		void CWebServer::Cmd_GetDeviceValueOptions(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			std::vector<std::vector<std::string>> devresult;
			devresult = m_sql.safe_query("SELECT Type, SubType FROM DeviceStatus WHERE (ID=='%q')", idx.c_str());
			if (!devresult.empty())
			{
				int devType = std::stoi(devresult[0][0]);
				int devSubType = std::stoi(devresult[0][1]);
				std::vector<std::string> result;
				result = CBasePush::DropdownOptions(devType, devSubType);
				int ii = 0;
				for (const auto& ddOption : result)
				{
					root["result"][ii]["Value"] = ii + 1;
					root["result"][ii]["Wording"] = ddOption.c_str();
					ii++;
				}
			}
			root["status"] = "OK";
			root["title"] = "GetDeviceValueOptions";
		}

		void CWebServer::Cmd_GetDeviceValueOptionWording(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			std::string idx = request::findValue(&req, "idx");
			std::string pos = request::findValue(&req, "pos");
			if ((idx.empty()) || (pos.empty()))
				return;
			std::string wording;
			std::vector<std::vector<std::string>> devresult;
			devresult = m_sql.safe_query("SELECT Type, SubType FROM DeviceStatus WHERE (ID=='%q')", idx.c_str());
			if (!devresult.empty())
			{
				int devType = std::stoi(devresult[0][0]);
				int devSubType = std::stoi(devresult[0][1]);
				wording = CBasePush::DropdownOptionsValue(devType, devSubType, std::stoi(pos));
			}
			root["wording"] = wording;
			root["status"] = "OK";
			root["title"] = "GetDeviceValueOptions";
		}

		void CWebServer::Cmd_AddUserVariable(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				_log.Log(LOG_ERROR, "User: %s tried to add a uservariable!", session.username.c_str());
				return; // Only admin user allowed
			}
			std::string variablename = HTMLSanitizer::Sanitize(request::findValue(&req, "vname"));
			std::string variablevalue = request::findValue(&req, "vvalue");
			std::string variabletype = request::findValue(&req, "vtype");

			root["title"] = "AddUserVariable";
			root["status"] = "ERR";

			if (!std::isdigit(variabletype[0]))
			{
				stdlower(variabletype);
				if (variabletype == "integer")
					variabletype = "0";
				else if (variabletype == "float")
					variabletype = "1";
				else if (variabletype == "string")
					variabletype = "2";
				else if (variabletype == "date")
					variabletype = "3";
				else if (variabletype == "time")
					variabletype = "4";
				else
				{
					root["message"] = "Invalid variabletype " + variabletype;
					return;
				}
			}

			if ((variablename.empty()) || (variabletype.empty()) ||
				((variabletype != "0") && (variabletype != "1") && (variabletype != "2") && (variabletype != "3") && (variabletype != "4")) ||
				((variablevalue.empty()) && (variabletype != "2")))
			{
				root["message"] = "Invalid variabletype " + variabletype;
				return;
			}

			std::string errorMessage;
			if (!m_sql.AddUserVariable(variablename, (const _eUsrVariableType)atoi(variabletype.c_str()), variablevalue, errorMessage))
			{
				root["message"] = errorMessage;
			}
			else
			{
				root["status"] = "OK";
			}
		}

		void CWebServer::Cmd_DeleteUserVariable(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				_log.Log(LOG_ERROR, "User: %s tried to delete a uservariable!", session.username.c_str());
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;

			m_sql.DeleteUserVariable(idx);
			root["status"] = "OK";
			root["title"] = "DeleteUserVariable";
		}

		void CWebServer::Cmd_UpdateUserVariable(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				_log.Log(LOG_ERROR, "User: %s tried to update a uservariable!", session.username.c_str());
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			std::string variablename = HTMLSanitizer::Sanitize(request::findValue(&req, "vname"));
			std::string variablevalue = request::findValue(&req, "vvalue");
			std::string variabletype = request::findValue(&req, "vtype");

			root["title"] = "UpdateUserVariable";
			root["status"] = "ERR";

			if (!std::isdigit(variabletype[0]))
			{
				stdlower(variabletype);
				if (variabletype == "integer")
					variabletype = "0";
				else if (variabletype == "float")
					variabletype = "1";
				else if (variabletype == "string")
					variabletype = "2";
				else if (variabletype == "date")
					variabletype = "3";
				else if (variabletype == "time")
					variabletype = "4";
				else
				{
					root["message"] = "Invalid variabletype " + variabletype;
					return;
				}
			}

			if ((variablename.empty()) || (variabletype.empty()) ||
				((variabletype != "0") && (variabletype != "1") && (variabletype != "2") && (variabletype != "3") && (variabletype != "4")) ||
				((variablevalue.empty()) && (variabletype != "2")))
			{
				root["message"] = "Invalid variabletype " + variabletype;
				return;
			}

			std::vector<std::vector<std::string>> result;
			if (idx.empty())
			{
				result = m_sql.safe_query("SELECT ID FROM UserVariables WHERE Name='%q'", variablename.c_str());
				if (result.empty())
				{
					root["message"] = "Uservariable " + variablename + " does not exist";
					return;
				}
				idx = result[0][0];
			}

			result = m_sql.safe_query("SELECT Name, ValueType FROM UserVariables WHERE ID='%q'", idx.c_str());
			if (result.empty())
			{
				root["message"] = "Uservariable " + variablename + " does not exist";
				return;
			}

			bool bTypeNameChanged = false;
			if (variablename != result[0][0])
				bTypeNameChanged = true; // new name
			else if (variabletype != result[0][1])
				bTypeNameChanged = true; // new type

			std::string errorMessage;
			if (!m_sql.UpdateUserVariable(idx, variablename, (const _eUsrVariableType)atoi(variabletype.c_str()), variablevalue, !bTypeNameChanged, errorMessage))
			{
				root["message"] = errorMessage;
			}
			else
			{
				root["status"] = "OK";
				if (bTypeNameChanged)
				{
					if (m_sql.m_bEnableEventSystem)
						m_mainworker.m_eventsystem.GetCurrentUserVariables();
				}
			}
		}

		void CWebServer::Cmd_GetUserVariables(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT ID, Name, ValueType, Value, LastUpdate FROM UserVariables");
			int ii = 0;
			for (const auto& sd : result)
			{
				root["result"][ii]["idx"] = sd[0];
				root["result"][ii]["Name"] = sd[1];
				root["result"][ii]["Type"] = sd[2];
				root["result"][ii]["Value"] = sd[3];
				root["result"][ii]["LastUpdate"] = sd[4];
				ii++;
			}
			root["status"] = "OK";
			root["title"] = "GetUserVariables";
		}

		void CWebServer::Cmd_GetUserVariable(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;

			int iVarID = atoi(idx.c_str());

			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT ID, Name, ValueType, Value, LastUpdate FROM UserVariables WHERE (ID==%d)", iVarID);
			int ii = 0;
			for (const auto& sd : result)
			{
				root["result"][ii]["idx"] = sd[0];
				root["result"][ii]["Name"] = sd[1];
				root["result"][ii]["Type"] = sd[2];
				root["result"][ii]["Value"] = sd[3];
				root["result"][ii]["LastUpdate"] = sd[4];
				ii++;
			}
			root["status"] = "OK";
			root["title"] = "GetUserVariable";
		}

		void CWebServer::Cmd_AllowNewHardware(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			std::string sTimeout = request::findValue(&req, "timeout");
			if (sTimeout.empty())
				return;
			root["status"] = "OK";
			root["title"] = "AllowNewHardware";

			m_sql.AllowNewHardwareTimer(atoi(sTimeout.c_str()));
		}

		void CWebServer::Cmd_DeleteHardware(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			int hwID = atoi(idx.c_str());

			CDomoticzHardwareBase* pBaseHardware = m_mainworker.GetHardware(hwID);
			if ((pBaseHardware != nullptr) && (pBaseHardware->HwdType == HTYPE_DomoticzInternal))
			{
				// DomoticzInternal cannot be removed
				return;
			}

			root["status"] = "OK";
			root["title"] = "DeleteHardware";

			m_mainworker.RemoveDomoticzHardware(hwID);
			m_sql.DeleteHardware(idx);
		}

		void CWebServer::Cmd_GetLog(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "OK";
			root["title"] = "GetLog";

			time_t lastlogtime = 0;
			std::string slastlogtime = request::findValue(&req, "lastlogtime");
			if (!slastlogtime.empty())
			{
				std::stringstream s_str(slastlogtime);
				s_str >> lastlogtime;
			}

			_eLogLevel lLevel = LOG_NORM;
			std::string sloglevel = request::findValue(&req, "loglevel");
			if (!sloglevel.empty())
			{
				lLevel = (_eLogLevel)atoi(sloglevel.c_str());
			}

			std::list<CLogger::_tLogLineStruct> logmessages = _log.GetLog(lLevel);
			int ii = 0;
			for (const auto& msg : logmessages)
			{
				if (msg.logtime > lastlogtime)
				{
					std::stringstream szLogTime;
					szLogTime << msg.logtime;
					root["LastLogTime"] = szLogTime.str();
					root["result"][ii]["level"] = static_cast<int>(msg.level);
					root["result"][ii]["message"] = msg.logmessage;
					ii++;
				}
			}
		}

		void CWebServer::Cmd_ClearLog(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "OK";
			root["title"] = "ClearLog";
			_log.ClearLog();
		}

		// Plan Functions
		void CWebServer::Cmd_AddPlan(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string name = HTMLSanitizer::Sanitize(request::findValue(&req, "name"));
			if (name.empty())
			{
				session.reply_status = reply::bad_request;
			}

			root["status"] = "OK";
			root["title"] = "AddPlan";
			m_sql.safe_query("INSERT INTO Plans (Name) VALUES ('%q')", name.c_str());
			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT MAX(ID) FROM Plans");
			if (!result.empty())
			{
				std::vector<std::string> sd = result[0];
				int ID = atoi(sd[0].c_str());

				root["idx"] = ID; // OTO output the created ID for easier management on the caller side (if automated)
			}
		}

		void CWebServer::Cmd_UpdatePlan(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			std::string name = HTMLSanitizer::Sanitize(request::findValue(&req, "name"));
			if (name.empty())
			{
				session.reply_status = reply::bad_request;
				return;
			}

			root["status"] = "OK";
			root["title"] = "UpdatePlan";

			m_sql.safe_query("UPDATE Plans SET Name='%q' WHERE (ID == '%q')", name.c_str(), idx.c_str());
		}

		void CWebServer::Cmd_DeletePlan(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "DeletePlan";
			m_sql.safe_query("DELETE FROM DeviceToPlansMap WHERE (PlanID == '%q')", idx.c_str());
			m_sql.safe_query("DELETE FROM Plans WHERE (ID == '%q')", idx.c_str());
		}

		void CWebServer::Cmd_GetUnusedPlanDevices(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "OK";
			root["title"] = "GetUnusedPlanDevices";
			std::string sunique = request::findValue(&req, "unique");
			std::string sactplan = request::findValue(&req, "actplan");
			if (
				sunique.empty()
				|| sactplan.empty()
				)
				return;
			const int iActPlan = atoi(sactplan.c_str());
			const bool iUnique = (sunique == "true") ? true : false;
			int ii = 0;

			std::vector<std::vector<std::string>> result;
			std::vector<std::vector<std::string>> result2;
			result = m_sql.safe_query("SELECT T1.[ID], T1.[Name], T1.[Type], T1.[SubType], T2.[Name] AS HardwareName FROM DeviceStatus as T1, Hardware as T2 "
				"WHERE (T1.[Used]==1) AND (T2.[ID]==T1.[HardwareID]) ORDER BY T2.[Name], T1.[Name]");
			if (!result.empty())
			{
				for (const auto& sd : result)
				{
					bool bDoAdd = true;
					if (iUnique)
					{
						result2 = m_sql.safe_query("SELECT ID FROM DeviceToPlansMap WHERE (DeviceRowID=='%q') AND (DevSceneType==0) AND (PlanID==%d)", sd[0].c_str(), iActPlan);
						bDoAdd = result2.empty();
					}
					if (bDoAdd)
					{
						int _dtype = atoi(sd[2].c_str());
						std::string Name = "[" + sd[4] + "] " + sd[1] + " (" + RFX_Type_Desc(_dtype, 1) + "/" + RFX_Type_SubType_Desc(_dtype, atoi(sd[3].c_str())) + ")";
						root["result"][ii]["type"] = 0;
						root["result"][ii]["idx"] = sd[0];
						root["result"][ii]["Name"] = Name;
						ii++;
					}
				}
			}
			// Add Scenes
			result = m_sql.safe_query("SELECT ID, Name FROM Scenes ORDER BY Name COLLATE NOCASE ASC");
			if (!result.empty())
			{
				for (const auto& sd : result)
				{
					bool bDoAdd = true;
					if (iUnique)
					{
						result2 = m_sql.safe_query("SELECT ID FROM DeviceToPlansMap WHERE (DeviceRowID=='%q') AND (DevSceneType==1) AND (PlanID==%d)", sd[0].c_str(), iActPlan);
						bDoAdd = (result2.empty());
					}
					if (bDoAdd)
					{
						root["result"][ii]["type"] = 1;
						root["result"][ii]["idx"] = sd[0];
						std::string sname = "[Scene] " + sd[1];
						root["result"][ii]["Name"] = sname;
						ii++;
					}
				}
			}
		}

		void CWebServer::Cmd_AddPlanActiveDevice(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			std::string sactivetype = request::findValue(&req, "activetype");
			std::string activeidx = request::findValue(&req, "activeidx");
			if ((idx.empty()) || (sactivetype.empty()) || (activeidx.empty()))
				return;
			root["status"] = "OK";
			root["title"] = "AddPlanActiveDevice";

			int activetype = atoi(sactivetype.c_str());

			// check if it is not already there
			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT ID FROM DeviceToPlansMap WHERE (DeviceRowID=='%q') AND (DevSceneType==%d) AND (PlanID=='%q')", activeidx.c_str(), activetype, idx.c_str());
			if (result.empty())
			{
				m_sql.safe_query("INSERT INTO DeviceToPlansMap (DevSceneType,DeviceRowID, PlanID) VALUES (%d,'%q','%q')", activetype, activeidx.c_str(), idx.c_str());
			}
		}

		void CWebServer::Cmd_GetPlanDevices(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "GetPlanDevices";

			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT ID, DevSceneType, DeviceRowID, [Order] FROM DeviceToPlansMap WHERE (PlanID=='%q') ORDER BY [Order]", idx.c_str());
			if (!result.empty())
			{
				int ii = 0;
				for (const auto& sd : result)
				{
					std::string ID = sd[0];
					int DevSceneType = atoi(sd[1].c_str());
					std::string DevSceneRowID = sd[2];

					std::string Name;
					if (DevSceneType == 0)
					{
						std::vector<std::vector<std::string>> result2;
						result2 = m_sql.safe_query("SELECT Name FROM DeviceStatus WHERE (ID=='%q')", DevSceneRowID.c_str());
						if (!result2.empty())
						{
							Name = result2[0][0];
						}
					}
					else
					{
						std::vector<std::vector<std::string>> result2;
						result2 = m_sql.safe_query("SELECT Name FROM Scenes WHERE (ID=='%q')", DevSceneRowID.c_str());
						if (!result2.empty())
						{
							Name = "[Scene] " + result2[0][0];
						}
					}
					if (!Name.empty())
					{
						root["result"][ii]["idx"] = ID;
						root["result"][ii]["devidx"] = DevSceneRowID;
						root["result"][ii]["type"] = DevSceneType;
						root["result"][ii]["DevSceneRowID"] = DevSceneRowID;
						root["result"][ii]["order"] = sd[3];
						root["result"][ii]["Name"] = Name;
						ii++;
					}
				}
			}
		}

		void CWebServer::Cmd_DeletePlanDevice(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "DeletePlanDevice";
			m_sql.safe_query("DELETE FROM DeviceToPlansMap WHERE (ID == '%q')", idx.c_str());
		}

		void CWebServer::Cmd_SetPlanDeviceCoords(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string idx = request::findValue(&req, "idx");
			std::string planidx = request::findValue(&req, "planidx");
			std::string xoffset = request::findValue(&req, "xoffset");
			std::string yoffset = request::findValue(&req, "yoffset");
			std::string type = request::findValue(&req, "DevSceneType");
			if ((idx.empty()) || (planidx.empty()) || (xoffset.empty()) || (yoffset.empty()))
				return;
			if (type != "1")
				type = "0"; // 0 = Device, 1 = Scene/Group
			root["status"] = "OK";
			root["title"] = "SetPlanDeviceCoords";
			m_sql.safe_query("UPDATE DeviceToPlansMap SET [XOffset] = '%q', [YOffset] = '%q' WHERE (DeviceRowID='%q') and (PlanID='%q') and (DevSceneType='%q')", xoffset.c_str(),
				yoffset.c_str(), idx.c_str(), planidx.c_str(), type.c_str());
			_log.Log(LOG_STATUS, "(Floorplan) Device '%s' coordinates set to '%s,%s' in plan '%s'.", idx.c_str(), xoffset.c_str(), yoffset.c_str(), planidx.c_str());
		}

		void CWebServer::Cmd_DeleteAllPlanDevices(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "DeleteAllPlanDevices";
			m_sql.safe_query("DELETE FROM DeviceToPlansMap WHERE (PlanID == '%q')", idx.c_str());
		}

		void CWebServer::Cmd_ChangePlanOrder(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			std::string sway = request::findValue(&req, "way");
			if (sway.empty())
				return;
			bool bGoUp = (sway == "0");

			std::string aOrder, oID, oOrder;

			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT [Order] FROM Plans WHERE (ID=='%q')", idx.c_str());
			if (result.empty())
				return;
			aOrder = result[0][0];

			if (!bGoUp)
			{
				// Get next device order
				result = m_sql.safe_query("SELECT ID, [Order] FROM Plans WHERE ([Order]>'%q') ORDER BY [Order] ASC", aOrder.c_str());
				if (result.empty())
					return;
				oID = result[0][0];
				oOrder = result[0][1];
			}
			else
			{
				// Get previous device order
				result = m_sql.safe_query("SELECT ID, [Order] FROM Plans WHERE ([Order]<'%q') ORDER BY [Order] DESC", aOrder.c_str());
				if (result.empty())
					return;
				oID = result[0][0];
				oOrder = result[0][1];
			}
			// Swap them
			root["status"] = "OK";
			root["title"] = "ChangePlanOrder";

			m_sql.safe_query("UPDATE Plans SET [Order] = '%q' WHERE (ID='%q')", oOrder.c_str(), idx.c_str());
			m_sql.safe_query("UPDATE Plans SET [Order] = '%q' WHERE (ID='%q')", aOrder.c_str(), oID.c_str());
		}

		void CWebServer::Cmd_ChangePlanDeviceOrder(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string planid = request::findValue(&req, "planid");
			std::string idx = request::findValue(&req, "idx");
			std::string sway = request::findValue(&req, "way");
			if ((planid.empty()) || (idx.empty()) || (sway.empty()))
				return;
			bool bGoUp = (sway == "0");

			std::string aOrder, oID, oOrder;

			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT [Order] FROM DeviceToPlansMap WHERE ((ID=='%q') AND (PlanID=='%q'))", idx.c_str(), planid.c_str());
			if (result.empty())
				return;
			aOrder = result[0][0];

			if (!bGoUp)
			{
				// Get next device order
				result = m_sql.safe_query("SELECT ID, [Order] FROM DeviceToPlansMap WHERE (([Order]>'%q') AND (PlanID=='%q')) ORDER BY [Order] ASC", aOrder.c_str(), planid.c_str());
				if (result.empty())
					return;
				oID = result[0][0];
				oOrder = result[0][1];
			}
			else
			{
				// Get previous device order
				result = m_sql.safe_query("SELECT ID, [Order] FROM DeviceToPlansMap WHERE (([Order]<'%q') AND (PlanID=='%q')) ORDER BY [Order] DESC", aOrder.c_str(), planid.c_str());
				if (result.empty())
					return;
				oID = result[0][0];
				oOrder = result[0][1];
			}
			// Swap them
			root["status"] = "OK";
			root["title"] = "ChangePlanOrder";

			m_sql.safe_query("UPDATE DeviceToPlansMap SET [Order] = '%q' WHERE (ID='%q')", oOrder.c_str(), idx.c_str());
			m_sql.safe_query("UPDATE DeviceToPlansMap SET [Order] = '%q' WHERE (ID='%q')", aOrder.c_str(), oID.c_str());
		}

		void CWebServer::Cmd_GetVersion(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "OK";
			root["title"] = "GetVersion";
			if (session.rights != -1 )
			{
				root["version"] = szAppVersion;
				root["hash"] = szAppHash;
				root["build_time"] = szAppDate;
				CdzVents* dzvents = CdzVents::GetInstance();
				root["dzvents_version"] = dzvents->GetVersion();
				root["python_version"] = szPyVersion;
				root["UseUpdate"] = false;
				root["HaveUpdate"] = m_mainworker.IsUpdateAvailable(false);

				if (session.rights == URIGHTS_ADMIN)
				{
					root["UseUpdate"] = g_bUseUpdater;
					root["DomoticzUpdateURL"] = m_mainworker.m_szDomoticzUpdateURL;
					root["SystemName"] = m_mainworker.m_szSystemName;
					root["Revision"] = m_mainworker.m_iRevision;
				}
			}
		}

		void CWebServer::Cmd_GetAuth(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "OK";
			root["title"] = "GetAuth";
			if (session.rights != -1)
			{
				root["user"] = session.username;
				root["rights"] = session.rights;
				root["version"] = szAppVersion;
			}
		}

		void CWebServer::Cmd_GetMyProfile(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "ERR";
			root["title"] = "GetMyProfile";
			if (session.rights > 0)	// Viewer cannot change his profile
			{
				int iUser = FindUser(session.username.c_str());
				if (iUser != -1)
				{
					root["user"] = session.username;
					root["rights"] = session.rights;
					if (!m_users[iUser].Mfatoken.empty())
						root["mfasecret"] = m_users[iUser].Mfatoken;
					root["status"] = "OK";
				}
			}
		}

		void CWebServer::Cmd_UpdateMyProfile(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "ERR";
			root["title"] = "UpdateMyProfile";

			if (req.method == "POST" && session.rights > 0)	// Viewer cannot change his profile
			{
				std::string sUsername = request::findValue(&req, "username");
				int iUser = FindUser(session.username.c_str());
				if (iUser == -1)
				{
					root["error"] = "User not found!";
					return;
				}
				if (m_users[iUser].Username != sUsername)
				{
					root["error"] = "User mismatch!";
					return;
				}

				std::string sOldPwd = request::findValue(&req, "oldpwd");
				std::string sNewPwd = request::findValue(&req, "newpwd");
				if (!sOldPwd.empty() && !sNewPwd.empty())
				{
					if (m_users[iUser].Password == sOldPwd)
					{
						m_users[iUser].Password = sNewPwd;
						m_sql.safe_query("UPDATE Users SET Password='%q' WHERE (ID=%d)", sNewPwd.c_str(), m_users[iUser].ID);
						LoadUsers();	// Make sure the new password is loaded in memory
						root["status"] = "OK";
					}
					else
					{
						root["error"] = "Old password mismatch!";
						return;
					}
				}

				std::string sTotpsecret = request::findValue(&req, "totpsecret");
				std::string sTotpCode = request::findValue(&req, "totpcode");
				bool bEnablemfa = (request::findValue(&req, "enablemfa") == "true" ? true : false);
				if (bEnablemfa && sTotpsecret.empty())
				{
					root["error"] = "Not a valid TOTP secret!";
					return;
				}
				// Update the User Profile
				if (!bEnablemfa)
				{
					sTotpsecret = "";
				}
				else
				{
					//verify code
					if (!sTotpCode.empty())
					{
						std::string sTotpKey = "";
						if (base32_decode(sTotpsecret, sTotpKey))
						{
							if (!VerifySHA1TOTP(sTotpCode, sTotpKey))
							{
								root["error"] = "Incorrect/expired 6 digit code!";
								return;
							}
						}
					}
				}
				m_users[iUser].Mfatoken = sTotpsecret;
				m_sql.safe_query("UPDATE Users SET MFAsecret='%q' WHERE (ID=%d)", sTotpsecret.c_str(), m_users[iUser].ID);

				LoadUsers();
				root["status"] = "OK";
			}
		}

		void CWebServer::Cmd_GetUptime(WebEmSession& session, const request& req, Json::Value& root)
		{
			// this is used in the about page, we are going to round the seconds a bit to display nicer
			time_t atime = mytime(nullptr);
			time_t tuptime = atime - m_StartTime;
			// round to 5 seconds (nicer in about page)
			tuptime = ((tuptime / 5) * 5) + 5;
			int days, hours, minutes, seconds;
			days = (int)(tuptime / 86400);
			tuptime -= (days * 86400);
			hours = (int)(tuptime / 3600);
			tuptime -= (hours * 3600);
			minutes = (int)(tuptime / 60);
			tuptime -= (minutes * 60);
			seconds = (int)tuptime;
			root["status"] = "OK";
			root["title"] = "GetUptime";
			root["days"] = days;
			root["hours"] = hours;
			root["minutes"] = minutes;
			root["seconds"] = seconds;
		}

		void CWebServer::Cmd_GetActualHistory(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "OK";
			root["title"] = "GetActualHistory";

			std::string historyfile = szUserDataFolder + "History.txt";

			std::ifstream infile;
			int ii = 0;
			infile.open(historyfile.c_str());
			std::string sLine;
			if (infile.is_open())
			{
				while (!infile.eof())
				{
					getline(infile, sLine);
					root["LastLogTime"] = "";
					if (sLine.find("Version ") == 0)
						root["result"][ii]["level"] = 1;
					else
						root["result"][ii]["level"] = 0;
					root["result"][ii]["message"] = sLine;
					ii++;
				}
			}
		}

		void CWebServer::Cmd_GetNewHistory(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "OK";
			root["title"] = "GetNewHistory";

			std::string historyfile;
			int nValue;
			m_sql.GetPreferencesVar("ReleaseChannel", nValue);
			bool bIsBetaChannel = (nValue != 0);

			utsname my_uname;
			if (uname(&my_uname) < 0)
				return;

			std::string systemname = my_uname.sysname;
			std::string machine = my_uname.machine;
			std::transform(systemname.begin(), systemname.end(), systemname.begin(), ::tolower);

			if (machine == "armv6l" || (machine == "aarch64" && sizeof(void*) == 4))
			{
				// Seems like old arm systems can also use the new arm build
				machine = "armv7l";
			}

			std::string szHistoryURL = "https://www.domoticz.com/download.php?channel=stable&type=history";
			if (bIsBetaChannel)
			{
				if (((machine != "armv6l") && (machine != "armv7l") && (systemname != "windows") && (machine != "x86_64") && (machine != "aarch64")) ||
					(strstr(my_uname.release, "ARCH+") != nullptr))
					szHistoryURL = "https://www.domoticz.com/download.php?channel=beta&type=history";
				else
					szHistoryURL = "https://www.domoticz.com/download.php?channel=beta&type=history&system=" + systemname + "&machine=" + machine;
			}
			std::vector<std::string> ExtraHeaders;
			ExtraHeaders.push_back("Unique_ID: " + m_sql.m_UniqueID);
			ExtraHeaders.push_back("App_Version: " + szAppVersion);
			ExtraHeaders.push_back("App_Revision: " + std::to_string(iAppRevision));
			ExtraHeaders.push_back("System_Name: " + systemname);
			ExtraHeaders.push_back("Machine: " + machine);
			ExtraHeaders.push_back("Type: " + (!bIsBetaChannel) ? "Stable" : "Beta");

			if (!HTTPClient::GET(szHistoryURL, ExtraHeaders, historyfile))
			{
				historyfile = "Unable to get Online History document !!";
			}

			std::istringstream stream(historyfile);
			std::string sLine;
			int ii = 0;
			while (std::getline(stream, sLine))
			{
				root["LastLogTime"] = "";
				if (sLine.find("Version ") == 0)
					root["result"][ii]["level"] = 1;
				else
					root["result"][ii]["level"] = 0;
				root["result"][ii]["message"] = sLine;
				ii++;
			}
		}

		void CWebServer::Cmd_GetConfig(WebEmSession& session, const request& req, Json::Value& root)
		{
			Cmd_GetVersion(session, req, root);
			root["status"] = "ERR";
			root["title"] = "GetConfig";

			std::string sValue;
			int nValue = 0;
			int iDashboardType = 0;

			if (m_sql.GetPreferencesVar("Language", sValue))
			{
				root["language"] = sValue;
			}
			if (m_sql.GetPreferencesVar("DegreeDaysBaseTemperature", sValue))
			{
				root["DegreeDaysBaseTemperature"] = atof(sValue.c_str());
			}
			m_sql.GetPreferencesVar("DashboardType", iDashboardType);
			root["DashboardType"] = iDashboardType;
			m_sql.GetPreferencesVar("MobileType", nValue);
			root["MobileType"] = nValue;

			nValue = 1;
			m_sql.GetPreferencesVar("5MinuteHistoryDays", nValue);
			root["FiveMinuteHistoryDays"] = nValue;

			nValue = 1;
			m_sql.GetPreferencesVar("ShowUpdateEffect", nValue);
			root["result"]["ShowUpdatedEffect"] = (nValue == 1);

			root["AllowWidgetOrdering"] = m_sql.m_bAllowWidgetOrdering;

			root["WindScale"] = m_sql.m_windscale * 10.0F;
			root["WindSign"] = m_sql.m_windsign;
			root["TempScale"] = m_sql.m_tempscale;
			root["TempSign"] = m_sql.m_tempsign;

			int iUser = -1;
			if (!session.username.empty() && (iUser = FindUser(session.username.c_str())) != -1)
			{
				unsigned long UserID = m_users[iUser].ID;
				root["UserName"] = m_users[iUser].Username;

				int bEnableTabDashboard = 1;
				int bEnableTabFloorplans = 0;
				int bEnableTabLight = 1;
				int bEnableTabScenes = 1;
				int bEnableTabTemp = 1;
				int bEnableTabWeather = 1;
				int bEnableTabUtility = 1;
				int bEnableTabCustom = 0;

				std::vector<std::vector<std::string>> result;
				result = m_sql.safe_query("SELECT TabsEnabled FROM Users WHERE (ID==%lu)", UserID);
				if (!result.empty())
				{
					int TabsEnabled = atoi(result[0][0].c_str());
					bEnableTabLight = (TabsEnabled & (1 << 0));
					bEnableTabScenes = (TabsEnabled & (1 << 1));
					bEnableTabTemp = (TabsEnabled & (1 << 2));
					bEnableTabWeather = (TabsEnabled & (1 << 3));
					bEnableTabUtility = (TabsEnabled & (1 << 4));
					bEnableTabCustom = (TabsEnabled & (1 << 5));
					bEnableTabFloorplans = (TabsEnabled & (1 << 6));
				}

				if (iDashboardType == 3)
				{
					// Floorplan , no need to show a tab floorplan
					bEnableTabFloorplans = 0;
				}
				root["result"]["EnableTabDashboard"] = bEnableTabDashboard != 0;
				root["result"]["EnableTabFloorplans"] = bEnableTabFloorplans != 0;
				root["result"]["EnableTabLights"] = bEnableTabLight != 0;
				root["result"]["EnableTabScenes"] = bEnableTabScenes != 0;
				root["result"]["EnableTabTemp"] = bEnableTabTemp != 0;
				root["result"]["EnableTabWeather"] = bEnableTabWeather != 0;
				root["result"]["EnableTabUtility"] = bEnableTabUtility != 0;
				root["result"]["EnableTabCustom"] = bEnableTabCustom != 0;

				if (bEnableTabCustom)
				{
					// Add custom templates
					DIR* lDir;
					struct dirent* ent;
					std::string templatesFolder = szWWWFolder + "/templates";
					int iFile = 0;
					if ((lDir = opendir(templatesFolder.c_str())) != nullptr)
					{
						while ((ent = readdir(lDir)) != nullptr)
						{
							std::string filename = ent->d_name;
							size_t pos = filename.find(".htm");
							if (pos != std::string::npos)
							{
								std::string shortfile = filename.substr(0, pos);
								root["result"]["templates"][iFile]["file"] = shortfile;
								stdreplace(shortfile, "_", " ");
								root["result"]["templates"][iFile]["name"] = shortfile;
								iFile++;
								continue;
							}
							// Same thing for URLs
							pos = filename.find(".url");
							if (pos != std::string::npos)
							{
								std::string url;
								std::string shortfile = filename.substr(0, pos);
								// First get the URL from the file
								std::ifstream urlfile;
								urlfile.open((templatesFolder + "/" + filename).c_str());
								if (urlfile.is_open())
								{
									getline(urlfile, url);
									urlfile.close();
									// Pass URL in results
									stdreplace(shortfile, "_", " ");
									root["result"]["urls"][shortfile] = url;
								}
							}
						}
						closedir(lDir);
					}
				}
			}
			root["status"] = "OK";
		}

		// Could now be obsolete as only 1 usage was found in Forecast screen, which now uses other command
		void CWebServer::Cmd_GetLocation(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights == -1)
			{
				session.reply_status = reply::forbidden;
				return; // Only auth user allowed
			}
			root["title"] = "GetLocation";
			std::string Latitude = "1";
			std::string Longitude = "1";
			std::string sValue;
			if (m_sql.GetPreferencesVar("Location", sValue))
			{
				std::vector<std::string> strarray;
				StringSplit(sValue, ";", strarray);

				if (strarray.size() == 2)
				{
					Latitude = strarray[0];
					Longitude = strarray[1];
					root["status"] = "OK";
				}
			}
			root["Latitude"] = Latitude;
			root["Longitude"] = Longitude;
		}

		void CWebServer::Cmd_GetForecastConfig(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights == -1)
			{
				session.reply_status = reply::forbidden;
				return; // Only auth user allowed
			}

			std::string Latitude = "1";
			std::string Longitude = "1";
			std::string sValue, sFURL, forecast_url;
			std::stringstream ss, sURL;
			uint8_t iSucces = 0;
			bool iFrame = true;

			root["title"] = "GetForecastConfig";
			root["status"] = "Error";

			if (m_sql.GetPreferencesVar("Location", sValue))
			{
				std::vector<std::string> strarray;
				StringSplit(sValue, ";", strarray);

				if (strarray.size() == 2)
				{
					Latitude = strarray[0];
					Longitude = strarray[1];
					iSucces++;
				}
				root["Latitude"] = Latitude;
				root["Longitude"] = Longitude;
				sValue = "";
				sValue.clear();
			}

			root["Forecasthardware"] = 0;
			int iValue = 0;
			if (m_sql.GetPreferencesVar("ForecastHardwareID", iValue))
			{
				root["Forecasthardware"] = iValue;
			}

			if (root["Forecasthardware"] > 0)
			{
				int iHardwareID = root["Forecasthardware"].asInt();
				CDomoticzHardwareBase* pHardware = m_mainworker.GetHardware(iHardwareID);
				if (pHardware != nullptr)
				{
					if (pHardware->HwdType == HTYPE_OpenWeatherMap)
					{
						root["Forecasthardwaretype"] = HTYPE_OpenWeatherMap;
						COpenWeatherMap* pWHardware = dynamic_cast<COpenWeatherMap*>(pHardware);
						forecast_url = pWHardware->GetForecastURL();
						if (!forecast_url.empty())
						{
							sFURL = forecast_url;
							iFrame = false;
						}
						Json::Value forecast_data = pWHardware->GetForecastData();
						if (!forecast_data.empty())
						{
							root["Forecastdata"] = forecast_data;
						}
					}
					else if (pHardware->HwdType == HTYPE_BuienRadar)
					{
						root["Forecasthardwaretype"] = HTYPE_BuienRadar;
						CBuienRadar* pWHardware = dynamic_cast<CBuienRadar*>(pHardware);
						forecast_url = pWHardware->GetForecastURL();
						if (!forecast_url.empty())
						{
							sFURL = forecast_url;
						}
					}
					else if (pHardware->HwdType == HTYPE_VisualCrossing)
					{
						root["Forecasthardwaretype"] = HTYPE_VisualCrossing;
						CVisualCrossing* pWHardware = dynamic_cast<CVisualCrossing*>(pHardware);
						forecast_url = pWHardware->GetForecastURL();
						if (!forecast_url.empty())
						{
							sFURL = forecast_url;
						}
					}
					else
					{
						root["Forecasthardware"] = 0; // reset to 0
					}
				}
				else
				{
					_log.Debug(DEBUG_WEBSERVER, "CWebServer::GetForecastConfig() : Could not find hardware (not active?) for ID %s!", root["Forecasthardware"].asString().c_str());
					root["Forecasthardware"] = 0; // reset to 0
				}
			}

			if (root["Forecasthardware"] == 0 && iSucces == 1)
			{
				// No forecast device, but we have geo coords, so enough for fallback
				iSucces++;
			}
			else if (!sFURL.empty())
			{
				root["Forecasturl"] = sFURL;
				iSucces++;
			}

			if (iSucces == 2)
			{
				root["status"] = "OK";
			}
		}

		void CWebServer::Cmd_SendNotification(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string subject = request::findValue(&req, "subject");
			std::string body = request::findValue(&req, "body");
			std::string subsystem = request::findValue(&req, "subsystem");
			std::string extradata = request::findValue(&req, "extradata");
			if ((subject.empty()) || (body.empty()))
				return;
			if (subsystem.empty())
				subsystem = NOTIFYALL;
			// Add to queue
			if (m_notifications.SendMessage(0, std::string(""), subsystem, std::string(""), subject, body, extradata, 1, std::string(""), false))
			{
				root["status"] = "OK";
			}
			root["title"] = "SendNotification";
		}

		void CWebServer::Cmd_EmailCameraSnapshot(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string camidx = request::findValue(&req, "camidx");
			std::string subject = request::findValue(&req, "subject");
			if ((camidx.empty()) || (subject.empty()))
				return;
			// Add to queue
			m_sql.AddTaskItem(_tTaskItem::EmailCameraSnapshot(1, camidx, subject));
			root["status"] = "OK";
			root["title"] = "Email Camera Snapshot";
		}

		void CWebServer::Cmd_UpdateDevice(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string Username = "Admin";
			if (!session.username.empty())
				Username = session.username;

			if (session.rights < 1)
			{
				session.reply_status = reply::forbidden;
				return; // only user or higher allowed
			}

			std::string idx = request::findValue(&req, "idx");

			if (!IsIdxForUser(&session, atoi(idx.c_str())))
			{
				_log.Log(LOG_ERROR, "User: %s tried to update an Unauthorized device!", session.username.c_str());
				session.reply_status = reply::forbidden;
				return;
			}

			std::string hid = request::findValue(&req, "hid");
			std::string did = request::findValue(&req, "did");
			std::string dunit = request::findValue(&req, "dunit");
			std::string dtype = request::findValue(&req, "dtype");
			std::string dsubtype = request::findValue(&req, "dsubtype");

			std::string nvalue = request::findValue(&req, "nvalue");
			std::string svalue = request::findValue(&req, "svalue");
			std::string ptrigger = request::findValue(&req, "parsetrigger");

			bool parseTrigger = (ptrigger != "false");

			if ((nvalue.empty() && svalue.empty()))
			{
				return;
			}

			int signallevel = 12;
			int batterylevel = 255;

			if (idx.empty())
			{
				// No index supplied, check if raw parameters where supplied
				if ((hid.empty()) || (did.empty()) || (dunit.empty()) || (dtype.empty()) || (dsubtype.empty()))
					return;
			}
			else
			{
				// Get the raw device parameters
				std::vector<std::vector<std::string>> result;
				result = m_sql.safe_query("SELECT HardwareID, DeviceID, Unit, Type, SubType FROM DeviceStatus WHERE (ID=='%q')", idx.c_str());
				if (result.empty())
					return;
				hid = result[0][0];
				did = result[0][1];
				dunit = result[0][2];
				dtype = result[0][3];
				dsubtype = result[0][4];
			}

			int HardwareID = atoi(hid.c_str());
			std::string DeviceID = did;
			int unit = atoi(dunit.c_str());
			int devType = atoi(dtype.c_str());
			int subType = atoi(dsubtype.c_str());

			// uint64_t ulIdx = std::stoull(idx);

			int invalue = atoi(nvalue.c_str());

			std::string sSignalLevel = request::findValue(&req, "rssi");
			if (!sSignalLevel.empty())
			{
				signallevel = atoi(sSignalLevel.c_str());
			}
			std::string sBatteryLevel = request::findValue(&req, "battery");
			if (!sBatteryLevel.empty())
			{
				batterylevel = atoi(sBatteryLevel.c_str());
			}
			std::string szUpdateUser = Username + " (IP: " + session.remote_host + ")";
			if (m_mainworker.UpdateDevice(HardwareID, DeviceID, unit, devType, subType, invalue, svalue, szUpdateUser, signallevel, batterylevel, parseTrigger))
			{
				root["status"] = "OK";
				root["title"] = "Update Device";
			}
		}

		void CWebServer::Cmd_UpdateDevices(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string script = request::findValue(&req, "script");
			if (script.empty())
			{
				return;
			}
			std::string content = req.content;

			std::vector<std::string> allParameters;

			// Keep the url content on the right of the '?'
			std::vector<std::string> allParts;
			StringSplit(req.uri, "?", allParts);
			if (!allParts.empty())
			{
				// Split all url parts separated by a '&'
				StringSplit(allParts[1], "&", allParameters);
			}

			CLuaHandler luaScript;
			bool ret = luaScript.executeLuaScript(script, content, allParameters);
			if (ret)
			{
				root["status"] = "OK";
				root["title"] = "Update Device";
			}
		}

		void CWebServer::Cmd_CustomEvent(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights < 1)
			{
				session.reply_status = reply::forbidden;
				return; // only user or higher allowed
			}
			Json::Value eventInfo;
			eventInfo["name"] = request::findValue(&req, "event");
			if (!req.content.empty())
				eventInfo["data"] = req.content.c_str(); // data from POST
			else
				eventInfo["data"] = request::findValue(&req, "data"); // data in URL

			if (eventInfo["name"].empty())
			{
				return;
			}

			m_mainworker.m_notificationsystem.Notify(Notification::DZ_CUSTOM, Notification::STATUS_INFO, JSonToRawString(eventInfo));

			root["status"] = "OK";
			root["title"] = "Custom Event";
		}

		void CWebServer::Cmd_SetThermostatState(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string sstate = request::findValue(&req, "state");
			std::string idx = request::findValue(&req, "idx");
			std::string name = HTMLSanitizer::Sanitize(request::findValue(&req, "name"));

			if ((idx.empty()) || (sstate.empty()))
				return;
			int iState = atoi(sstate.c_str());

			int urights = 3;
			bool bHaveUser = (!session.username.empty());
			if (bHaveUser)
			{
				int iUser = FindUser(session.username.c_str());
				if (iUser != -1)
				{
					urights = static_cast<int>(m_users[iUser].userrights);
					_log.Log(LOG_STATUS, "User: %s initiated a Thermostat State change command", m_users[iUser].Username.c_str());
				}
			}
			if (urights < 1)
				return;

			root["status"] = "OK";
			root["title"] = "Set Thermostat State";
			_log.Log(LOG_NORM, "Setting Thermostat State....");
			m_mainworker.SetThermostatState(idx, iState);
		}

		void CWebServer::Cmd_SystemShutdown(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
#ifdef WIN32
			int ret = system("shutdown -s -f -t 1 -d up:125:1");
#else
			int ret = system("sudo shutdown -h now");
#endif
			if (ret != 0)
			{
				_log.Log(LOG_ERROR, "Error executing shutdown command. returned: %d", ret);
				return;
			}
			root["title"] = "SystemShutdown";
			root["status"] = "OK";
		}

		void CWebServer::Cmd_SystemReboot(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
#ifdef WIN32
			int ret = system("shutdown -r -f -t 1 -d up:125:1");
#else
			int ret = system("sudo shutdown -r now");
#endif
			if (ret != 0)
			{
				_log.Log(LOG_ERROR, "Error executing reboot command. returned: %d", ret);
				return;
			}
			root["title"] = "SystemReboot";
			root["status"] = "OK";
		}

		void CWebServer::Cmd_ExcecuteScript(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			std::string scriptname = request::findValue(&req, "scriptname");
			if (scriptname.empty())
				return;
			if (scriptname.find("..") != std::string::npos)
				return;
#ifdef WIN32
			scriptname = szUserDataFolder + "scripts\\" + scriptname;
#else
			scriptname = szUserDataFolder + "scripts/" + scriptname;
#endif
			if (!file_exist(scriptname.c_str()))
				return;
			std::string script_params = request::findValue(&req, "scriptparams");
			std::string strparm = szUserDataFolder;
			if (!script_params.empty())
			{
				if (!strparm.empty())
					strparm += " " + script_params;
				else
					strparm = script_params;
			}
			std::string sdirect = request::findValue(&req, "direct");
			if (sdirect == "true")
			{
				_log.Log(LOG_STATUS, "Executing script: %s", scriptname.c_str());
#ifdef WIN32
				ShellExecute(NULL, "open", scriptname.c_str(), strparm.c_str(), NULL, SW_SHOWNORMAL);
#else
				std::string lscript = scriptname + " " + strparm;
				int ret = system(lscript.c_str());
				if (ret != 0)
				{
					_log.Log(LOG_ERROR, "Error executing script command (%s). returned: %d", lscript.c_str(), ret);
					return;
				}
#endif
			}
			else
			{
				// add script to background worker
				m_sql.AddTaskItem(_tTaskItem::ExecuteScript(0.2F, scriptname, strparm));
			}
			root["title"] = "ExecuteScript";
			root["status"] = "OK";
		}

		// Only for Unix systems
		void CWebServer::Cmd_UpdateApplication(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
#ifdef WIN32
#ifndef _DEBUG
			return;
#endif
#endif
			int nValue;
			m_sql.GetPreferencesVar("ReleaseChannel", nValue);
			bool bIsBetaChannel = (nValue != 0);

			std::string scriptname(szStartupFolder);
			scriptname += (bIsBetaChannel) ? "updatebeta" : "updaterelease";
			// run script in background
			std::string lscript = scriptname + " &";
			int ret = system(lscript.c_str());
			root["title"] = "UpdateApplication";
			root["status"] = "OK";
		}

		void CWebServer::Cmd_GetCosts(WebEmSession& session, const request& req, Json::Value& root)
		{
			int nValue = 0;
			m_sql.GetPreferencesVar("CostEnergy", nValue);
			root["CostEnergy"] = nValue;
			m_sql.GetPreferencesVar("CostEnergyT2", nValue);
			root["CostEnergyT2"] = nValue;
			m_sql.GetPreferencesVar("CostEnergyR1", nValue);
			root["CostEnergyR1"] = nValue;
			m_sql.GetPreferencesVar("CostEnergyR2", nValue);
			root["CostEnergyR2"] = nValue;
			m_sql.GetPreferencesVar("CostGas", nValue);
			root["CostGas"] = nValue;
			m_sql.GetPreferencesVar("CostWater", nValue);
			root["CostWater"] = nValue;

			int tValue = 1000;
			if (m_sql.GetPreferencesVar("MeterDividerWater", tValue))
			{
				root["DividerWater"] = float(tValue);
			}
			float EnergyDivider = 1000.0F;
			if (m_sql.GetPreferencesVar("MeterDividerEnergy", tValue))
			{
				EnergyDivider = float(tValue);
				root["DividerEnergy"] = EnergyDivider;
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;

			char szTmp[100];
			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT Type, SubType, nValue, sValue FROM DeviceStatus WHERE (ID=='%q')", idx.c_str());
			if (!result.empty())
			{
				root["status"] = "OK";
				root["title"] = "GetCosts";

				std::vector<std::string> sd = result[0];
				unsigned char dType = atoi(sd[0].c_str());
				// unsigned char subType = atoi(sd[1].c_str());
				// nValue = (unsigned char)atoi(sd[2].c_str());
				std::string sValue = sd[3];

				if (dType == pTypeP1Power)
				{
					// also provide the counter values

					std::vector<std::string> splitresults;
					StringSplit(sValue, ";", splitresults);
					if (splitresults.size() != 6)
						return;

					uint64_t powerusage1 = std::stoull(splitresults[0]);
					uint64_t powerusage2 = std::stoull(splitresults[1]);
					uint64_t powerdeliv1 = std::stoull(splitresults[2]);
					uint64_t powerdeliv2 = std::stoull(splitresults[3]);
					// uint64_t usagecurrent = std::stoull(splitresults[4]);
					// uint64_t delivcurrent = std::stoull(splitresults[5]);

					powerdeliv1 = (powerdeliv1 < 10) ? 0 : powerdeliv1;
					powerdeliv2 = (powerdeliv2 < 10) ? 0 : powerdeliv2;

					sprintf(szTmp, "%.03f", float(powerusage1) / EnergyDivider);
					root["CounterT1"] = szTmp;
					sprintf(szTmp, "%.03f", float(powerusage2) / EnergyDivider);
					root["CounterT2"] = szTmp;
					sprintf(szTmp, "%.03f", float(powerdeliv1) / EnergyDivider);
					root["CounterR1"] = szTmp;
					sprintf(szTmp, "%.03f", float(powerdeliv2) / EnergyDivider);
					root["CounterR2"] = szTmp;
				}
			}
		}

		void CWebServer::Cmd_CheckForUpdate(WebEmSession& session, const request& req, Json::Value& root)
		{
			bool bHaveUser = (!session.username.empty());
			int urights = 3;
			if (bHaveUser)
			{
				int iUser = FindUser(session.username.c_str());
				if (iUser != -1)
					urights = static_cast<int>(m_users[iUser].userrights);
			}
			root["statuscode"] = urights;

			root["status"] = "OK";
			root["title"] = "CheckForUpdate";
			root["HaveUpdate"] = false;
			root["Revision"] = m_mainworker.m_iRevision;

			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin users may update
			}

			bool bIsForced = (request::findValue(&req, "forced") == "true");

			if (!bIsForced)
			{
				int nValue = 0;
				m_sql.GetPreferencesVar("UseAutoUpdate", nValue);
				if (nValue != 1)
				{
					return;
				}
			}

			root["HaveUpdate"] = m_mainworker.IsUpdateAvailable(bIsForced);
			root["DomoticzUpdateURL"] = m_mainworker.m_szDomoticzUpdateURL;
			root["SystemName"] = m_mainworker.m_szSystemName;
			root["Revision"] = m_mainworker.m_iRevision;
		}

		void CWebServer::Cmd_DownloadUpdate(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (!m_mainworker.StartDownloadUpdate())
				return;
			root["status"] = "OK";
			root["title"] = "DownloadUpdate";
		}

		void CWebServer::Cmd_DownloadReady(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (!m_mainworker.m_bHaveDownloadedDomoticzUpdate)
				return;
			root["status"] = "OK";
			root["title"] = "DownloadReady";
			root["downloadok"] = (m_mainworker.m_bHaveDownloadedDomoticzUpdateSuccessFull) ? true : false;
		}

		void CWebServer::Cmd_DeleteDateRange(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			const std::string idx = request::findValue(&req, "idx");
			const std::string fromDate = request::findValue(&req, "fromdate");
			const std::string toDate = request::findValue(&req, "todate");
			if ((idx.empty()) || (fromDate.empty() || toDate.empty()))
				return;
			root["status"] = "OK";
			root["title"] = "deletedaterange";
			m_sql.DeleteDateRange(idx.c_str(), fromDate, toDate);
		}

		void CWebServer::Cmd_DeleteDataPoint(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			const std::string idx = request::findValue(&req, "idx");
			const std::string Date = request::findValue(&req, "date");

			if ((idx.empty()) || (Date.empty()))
				return;

			root["status"] = "OK";
			root["title"] = "deletedatapoint";
			m_sql.DeleteDataPoint(idx.c_str(), Date);
		}

		bool CWebServer::IsIdxForUser(const WebEmSession* pSession, const int Idx)
		{
			if (pSession->rights == 2)
				return true;
			if (pSession->rights == 0)
				return false; // viewer
			// User
			int iUser = FindUser(pSession->username.c_str());
			if ((iUser < 0) || (iUser >= (int)m_users.size()))
				return false;

			if (m_users[iUser].TotSensors == 0)
				return true; // all sensors

			std::vector<std::vector<std::string>> result =
				m_sql.safe_query("SELECT DeviceRowID FROM SharedDevices WHERE (SharedUserID == '%d') AND (DeviceRowID == '%d')", m_users[iUser].ID, Idx);
			return (!result.empty());
		}

		void CWebServer::LoadUsers()
		{
			ClearUserPasswords();
			// Add Users
			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT ID, Active, Username, Password, MFAsecret, Rights, TabsEnabled FROM Users");
			if (!result.empty())
			{
				for (const auto& sd : result)
				{
					int bIsActive = static_cast<int>(atoi(sd[1].c_str()));
					if (bIsActive)
					{
						unsigned long ID = (unsigned long)atol(sd[0].c_str());

						std::string username = base64_decode(sd[2]);
						std::string password = sd[3];
						std::string mfatoken = sd[4];

						_eUserRights rights = (_eUserRights)atoi(sd[5].c_str());
						int activetabs = atoi(sd[6].c_str());

						AddUser(ID, username, password, mfatoken, rights, activetabs);
					}
				}
			}
			// Add 'Applications' as User with special privilege URIGHTS_CLIENTID
			result.clear();
			result = m_sql.safe_query("SELECT ID, Active, Public, Applicationname, Secret, Pemfile FROM Applications");
			if (!result.empty())
			{
				for (const auto& sd : result)
				{
					int bIsActive = static_cast<int>(atoi(sd[1].c_str()));
					if (bIsActive)
					{
						unsigned long ID = (unsigned long)m_iamsettings.getUserIdxOffset() + (unsigned long)atol(sd[0].c_str());
						int bPublic = static_cast<int>(atoi(sd[2].c_str()));
						std::string applicationname = sd[3];
						std::string secret = sd[4];
						std::string pemfile = sd[5];
						if (bPublic && secret.empty())
							secret = GenerateMD5Hash(pemfile);
						AddUser(ID, applicationname, secret, "", URIGHTS_CLIENTID, bPublic, pemfile);
					}
				}
			}

			m_mainworker.LoadSharedUsers();
		}

		void CWebServer::AddUser(const unsigned long ID, const std::string& username, const std::string& password, const std::string& mfatoken, const int userrights, const int activetabs, const std::string& pemfile)
		{
			if (m_pWebEm == nullptr)
				return;
			std::vector<std::vector<std::string>> result = m_sql.safe_query("SELECT COUNT(*) FROM SharedDevices WHERE (SharedUserID == '%d')", ID);
			if (result.empty())
				return;

			// Let's see if we can load the public/private keyfile for this user/client
			std::string privkey = "";
			std::string pubkey = "";
			if (!pemfile.empty())
			{
				std::string sErr = "";
				std::ifstream ifs;

				std::string szTmpFile = szUserDataFolder + pemfile;

				ifs.open(szTmpFile);
				if (ifs.is_open())
				{
					std::string sLine = "";
					int i = 0;
					bool bPriv = false;
					bool bPrivFound = false;
					bool bPub = false;
					bool bPubFound = false;
					while (std::getline(ifs, sLine))
					{
						sLine += '\n';	// Newlines need to be added so the SSL library understands the Public/Private keys
						if (sLine.find("-----BEGIN PUBLIC KEY") != std::string::npos)
						{
							bPub = true;
						}
						if (sLine.find("-----BEGIN PRIVATE KEY") != std::string::npos)
						{
							bPriv = true;
						}
						if (bPriv)
							privkey += sLine;
						if (bPub)
							pubkey += sLine;
						if (sLine.find("-----END PUBLIC KEY") != std::string::npos)
						{
							if (bPub)
								bPubFound = true;
							bPub = false;
						}
						if (sLine.find("-----END PRIVATE KEY") != std::string::npos)
						{
							if (bPriv)
								bPrivFound = true;
							bPriv = false;
						}
						i++;
					}
					_log.Debug(DEBUG_AUTH, "Add User: Found PEMfile (%s) for User (%s) with %d lines. PubKey (%d), PrivKey (%d)", szTmpFile.c_str(), username.c_str(), i, bPubFound, bPrivFound);
					ifs.close();
					if (!bPubFound)
						sErr = "Unable to find a Public key within the PEMfile";
					else if (!bPrivFound)
						_log.Log(LOG_STATUS, "AddUser: Pemfile (%s) only has a Public key, so only verification is possible. Token generation has to be done external.", szTmpFile.c_str());
				}
				else
					sErr = "Unable to find/open file";

				if (!sErr.empty())
				{
					_log.Log(LOG_STATUS, "AddUser: Unable to load and process given PEMfile (%s) (%s)!", szTmpFile.c_str(), sErr.c_str());
					return;
				}
			}

			_tWebUserPassword wtmp;
			wtmp.ID = ID;
			wtmp.Username = username;
			wtmp.Password = password;
			wtmp.Mfatoken = mfatoken;
			wtmp.PrivKey = privkey;
			wtmp.PubKey = pubkey;
			wtmp.userrights = (_eUserRights)userrights;
			wtmp.ActiveTabs = activetabs;
			wtmp.TotSensors = atoi(result[0][0].c_str());
			m_users.push_back(wtmp);

			_tUserAccessCode utmp;
			utmp.ID = ID;
			utmp.UserName = username;
			utmp.clientID = -1;
			utmp.ExpTime = 0;
			utmp.AuthCode = "";
			utmp.Scope = "";
			utmp.RedirectUri = "";
			m_accesscodes.push_back(utmp);

			m_pWebEm->AddUserPassword(ID, username, password, mfatoken, (_eUserRights)userrights, activetabs, privkey, pubkey);
		}

		void CWebServer::ClearUserPasswords()
		{
			m_users.clear();
			m_accesscodes.clear();
			if (m_pWebEm)
				m_pWebEm->ClearUserPasswords();
		}

		int CWebServer::FindUser(const char* szUserName)
		{
			int iUser = 0;
			for (const auto& user : m_users)
			{
				if (user.Username == szUserName)
					return iUser;
				iUser++;
			}
			return -1;
		}

		bool CWebServer::FindAdminUser()
		{
			return std::any_of(m_users.begin(), m_users.end(), [](const _tWebUserPassword& user) { return user.userrights == URIGHTS_ADMIN; });
		}

		int CWebServer::CountAdminUsers()
		{
			int iAdmins = 0;
			for (const auto& user : m_users)
			{
				if (user.userrights == URIGHTS_ADMIN)
					iAdmins++;
			}
			return iAdmins;
		}

		// PostSettings
		void CWebServer::Cmd_PostSettings(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			root["title"] = "StoreSettings";
			root["status"] = "ERR";

			uint16_t cntSettings = 0;

			try {

				/* Start processing the simple ones */
				/* -------------------------------- */

				m_sql.UpdatePreferencesVar("DashboardType", atoi(request::findValue(&req, "DashboardType").c_str())); cntSettings++;
				m_sql.UpdatePreferencesVar("MobileType", atoi(request::findValue(&req, "MobileType").c_str())); cntSettings++;
				m_sql.UpdatePreferencesVar("ReleaseChannel", atoi(request::findValue(&req, "ReleaseChannel").c_str())); cntSettings++;
				m_sql.UpdatePreferencesVar("LightHistoryDays", atoi(request::findValue(&req, "LightHistoryDays").c_str())); cntSettings++;
				m_sql.UpdatePreferencesVar("5MinuteHistoryDays", atoi(request::findValue(&req, "ShortLogDays").c_str())); cntSettings++;
				m_sql.UpdatePreferencesVar("ElectricVoltage", atoi(request::findValue(&req, "ElectricVoltage").c_str())); cntSettings++;
				m_sql.UpdatePreferencesVar("CM113DisplayType", atoi(request::findValue(&req, "CM113DisplayType").c_str())); cntSettings++;
				m_sql.UpdatePreferencesVar("MaxElectricPower", atoi(request::findValue(&req, "MaxElectricPower").c_str())); cntSettings++;
				m_sql.UpdatePreferencesVar("DoorbellCommand", atoi(request::findValue(&req, "DoorbellCommand").c_str())); cntSettings++;
				m_sql.UpdatePreferencesVar("SmartMeterType", atoi(request::findValue(&req, "SmartMeterType").c_str())); cntSettings++;
				m_sql.UpdatePreferencesVar("SecOnDelay", atoi(request::findValue(&req, "SecOnDelay").c_str())); cntSettings++;
				m_sql.UpdatePreferencesVar("FloorplanPopupDelay", atoi(request::findValue(&req, "FloorplanPopupDelay").c_str())); cntSettings++;
				m_sql.UpdatePreferencesVar("FloorplanActiveOpacity", atoi(request::findValue(&req, "FloorplanActiveOpacity").c_str())); cntSettings++;
				m_sql.UpdatePreferencesVar("FloorplanInactiveOpacity", atoi(request::findValue(&req, "FloorplanInactiveOpacity").c_str())); cntSettings++;
				m_sql.UpdatePreferencesVar("OneWireSensorPollPeriod", atoi(request::findValue(&req, "OneWireSensorPollPeriod").c_str())); cntSettings++;
				m_sql.UpdatePreferencesVar("OneWireSwitchPollPeriod", atoi(request::findValue(&req, "OneWireSwitchPollPeriod").c_str())); cntSettings++;

				m_sql.UpdatePreferencesVar("UseAutoUpdate", (request::findValue(&req, "checkforupdates") == "on" ? 1 : 0)); cntSettings++;
				m_sql.UpdatePreferencesVar("UseAutoBackup", (request::findValue(&req, "enableautobackup") == "on" ? 1 : 0)); cntSettings++;
				m_sql.UpdatePreferencesVar("HideDisabledHardwareSensors", (request::findValue(&req, "HideDisabledHardwareSensors") == "on" ? 1 : 0)); cntSettings++;
				m_sql.UpdatePreferencesVar("ShowUpdateEffect", (request::findValue(&req, "ShowUpdateEffect") == "on" ? 1 : 0)); cntSettings++;
				m_sql.UpdatePreferencesVar("FloorplanFullscreenMode", (request::findValue(&req, "FloorplanFullscreenMode") == "on" ? 1 : 0)); cntSettings++;
				m_sql.UpdatePreferencesVar("FloorplanAnimateZoom", (request::findValue(&req, "FloorplanAnimateZoom") == "on" ? 1 : 0)); cntSettings++;
				m_sql.UpdatePreferencesVar("FloorplanShowSensorValues", (request::findValue(&req, "FloorplanShowSensorValues") == "on" ? 1 : 0)); cntSettings++;
				m_sql.UpdatePreferencesVar("FloorplanShowSwitchValues", (request::findValue(&req, "FloorplanShowSwitchValues") == "on" ? 1 : 0)); cntSettings++;
				m_sql.UpdatePreferencesVar("FloorplanShowSceneNames", (request::findValue(&req, "FloorplanShowSceneNames") == "on" ? 1 : 0)); cntSettings++;
				m_sql.UpdatePreferencesVar("IFTTTEnabled", (request::findValue(&req, "IFTTTEnabled") == "on" ? 1 : 0)); cntSettings++;

				m_sql.UpdatePreferencesVar("Language", request::findValue(&req, "Language")); cntSettings++;
				m_sql.UpdatePreferencesVar("DegreeDaysBaseTemperature", request::findValue(&req, "DegreeDaysBaseTemperature")); cntSettings++;

				m_sql.UpdatePreferencesVar("FloorplanRoomColour", CURLEncode::URLDecode(request::findValue(&req, "FloorplanRoomColour"))); cntSettings++;
				m_sql.UpdatePreferencesVar("IFTTTAPI", base64_encode(request::findValue(&req, "IFTTTAPI"))); cntSettings++;

				m_sql.UpdatePreferencesVar("Title", (request::findValue(&req, "Title").empty()) ? "Domoticz" : request::findValue(&req, "Title")); cntSettings++;

				/* More complex ones that need additional processing */
				/* ------------------------------------------------- */

				float CostEnergy = static_cast<float>(atof(request::findValue(&req, "CostEnergy").c_str()));
				m_sql.UpdatePreferencesVar("CostEnergy", int(CostEnergy * 10000.0F)); cntSettings++;
				float CostEnergyT2 = static_cast<float>(atof(request::findValue(&req, "CostEnergyT2").c_str()));
				m_sql.UpdatePreferencesVar("CostEnergyT2", int(CostEnergyT2 * 10000.0F)); cntSettings++;
				float CostEnergyR1 = static_cast<float>(atof(request::findValue(&req, "CostEnergyR1").c_str()));
				m_sql.UpdatePreferencesVar("CostEnergyR1", int(CostEnergyR1 * 10000.0F)); cntSettings++;
				float CostEnergyR2 = static_cast<float>(atof(request::findValue(&req, "CostEnergyR2").c_str()));
				m_sql.UpdatePreferencesVar("CostEnergyR2", int(CostEnergyR2 * 10000.0F)); cntSettings++;
				float CostGas = static_cast<float>(atof(request::findValue(&req, "CostGas").c_str()));
				m_sql.UpdatePreferencesVar("CostGas", int(CostGas * 10000.0F)); cntSettings++;
				float CostWater = static_cast<float>(atof(request::findValue(&req, "CostWater").c_str()));
				m_sql.UpdatePreferencesVar("CostWater", int(CostWater * 10000.0F)); cntSettings++;

				int EnergyDivider = atoi(request::findValue(&req, "EnergyDivider").c_str());
				if (EnergyDivider < 1)
					EnergyDivider = 1000;
				m_sql.UpdatePreferencesVar("MeterDividerEnergy", EnergyDivider); cntSettings++;
				int GasDivider = atoi(request::findValue(&req, "GasDivider").c_str());
				if (GasDivider < 1)
					GasDivider = 100;
				m_sql.UpdatePreferencesVar("MeterDividerGas", GasDivider); cntSettings++;
				int WaterDivider = atoi(request::findValue(&req, "WaterDivider").c_str());
				if (WaterDivider < 1)
					WaterDivider = 100;
				m_sql.UpdatePreferencesVar("MeterDividerWater", WaterDivider); cntSettings++;

				int sensortimeout = atoi(request::findValue(&req, "SensorTimeout").c_str());
				if (sensortimeout < 10)
					sensortimeout = 10;
				m_sql.UpdatePreferencesVar("SensorTimeout", sensortimeout); cntSettings++;

				std::string RaspCamParams = request::findValue(&req, "RaspCamParams");
				if ((!RaspCamParams.empty()) && (IsArgumentSecure(RaspCamParams)))
					m_sql.UpdatePreferencesVar("RaspCamParams", RaspCamParams);
				cntSettings++;

				std::string UVCParams = request::findValue(&req, "UVCParams");
				if ((!UVCParams.empty()) && (IsArgumentSecure(UVCParams)))
					m_sql.UpdatePreferencesVar("UVCParams", UVCParams);
				cntSettings++;

				/* Also update m_sql.variables */
				/* --------------------------- */

				int iShortLogInterval = atoi(request::findValue(&req, "ShortLogInterval").c_str());
				if (iShortLogInterval < 1)
					iShortLogInterval = 5;
				m_sql.m_ShortLogInterval = iShortLogInterval;
				m_sql.UpdatePreferencesVar("ShortLogInterval", m_sql.m_ShortLogInterval); cntSettings++;

				m_sql.m_bShortLogAddOnlyNewValues = (request::findValue(&req, "ShortLogAddOnlyNewValues") == "on" ? 1 : 0);
				m_sql.UpdatePreferencesVar("ShortLogAddOnlyNewValues", m_sql.m_bShortLogAddOnlyNewValues); cntSettings++;

				m_sql.m_bLogEventScriptTrigger = (request::findValue(&req, "LogEventScriptTrigger") == "on" ? 1 : 0);
				m_sql.UpdatePreferencesVar("LogEventScriptTrigger", m_sql.m_bLogEventScriptTrigger); cntSettings++;

				m_sql.m_bAllowWidgetOrdering = (request::findValue(&req, "AllowWidgetOrdering") == "on" ? 1 : 0);
				m_sql.UpdatePreferencesVar("AllowWidgetOrdering", m_sql.m_bAllowWidgetOrdering); cntSettings++;

				int iEnableNewHardware = (request::findValue(&req, "AcceptNewHardware") == "on" ? 1 : 0);
				m_sql.m_bAcceptNewHardware = (iEnableNewHardware == 1);
				m_sql.UpdatePreferencesVar("AcceptNewHardware", m_sql.m_bAcceptNewHardware); cntSettings++;

				int nUnit = atoi(request::findValue(&req, "WindUnit").c_str());
				m_sql.m_windunit = (_eWindUnit)nUnit;
				m_sql.UpdatePreferencesVar("WindUnit", m_sql.m_windunit); cntSettings++;

				nUnit = atoi(request::findValue(&req, "TempUnit").c_str());
				m_sql.m_tempunit = (_eTempUnit)nUnit;
				m_sql.UpdatePreferencesVar("TempUnit", m_sql.m_tempunit); cntSettings++;

				nUnit = atoi(request::findValue(&req, "WeightUnit").c_str());
				m_sql.m_weightunit = (_eWeightUnit)nUnit;
				m_sql.UpdatePreferencesVar("WeightUnit", m_sql.m_weightunit); cntSettings++;

				m_sql.SetUnitsAndScale();

				/* Update Preferences and call other functions as well due to changes */
				/* ------------------------------------------------------------------ */

				std::string Latitude = request::findValue(&req, "Latitude");
				std::string Longitude = request::findValue(&req, "Longitude");
				if ((!Latitude.empty()) && (!Longitude.empty()))
				{
					std::string LatLong = Latitude + ";" + Longitude;
					m_sql.UpdatePreferencesVar("Location", LatLong);
					m_mainworker.GetSunSettings();
				}
				cntSettings++;

				bool AllowPlainBasicAuth = (request::findValue(&req, "AllowPlainBasicAuth") == "on" ? 1 : 0);
				m_sql.UpdatePreferencesVar("AllowPlainBasicAuth", AllowPlainBasicAuth);

				m_pWebEm->SetAllowPlainBasicAuth(AllowPlainBasicAuth);
				cntSettings++;

				std::string WebLocalNetworks = CURLEncode::URLDecode(request::findValue(&req, "WebLocalNetworks"));
				m_sql.UpdatePreferencesVar("WebLocalNetworks", WebLocalNetworks);
				m_webservers.ReloadTrustedNetworks();
				cntSettings++;

				if (session.username.empty())
				{
					// Local network could be changed so lets force a check here
					session.rights = -1;
				}

				std::string SecPassword = request::findValue(&req, "SecPassword");
				SecPassword = CURLEncode::URLDecode(SecPassword);
				if (SecPassword.size() != 32)
				{
					SecPassword = GenerateMD5Hash(SecPassword);
				}
				m_sql.UpdatePreferencesVar("SecPassword", SecPassword);
				cntSettings++;

				std::string ProtectionPassword = request::findValue(&req, "ProtectionPassword");
				ProtectionPassword = CURLEncode::URLDecode(ProtectionPassword);
				if (ProtectionPassword.size() != 32)
				{
					ProtectionPassword = GenerateMD5Hash(ProtectionPassword);
				}
				m_sql.UpdatePreferencesVar("ProtectionPassword", ProtectionPassword);
				cntSettings++;

				std::string SendErrorsAsNotification = request::findValue(&req, "SendErrorsAsNotification");
				int iSendErrorsAsNotification = (SendErrorsAsNotification == "on" ? 1 : 0);
				m_sql.UpdatePreferencesVar("SendErrorsAsNotification", iSendErrorsAsNotification);
				_log.ForwardErrorsToNotificationSystem(iSendErrorsAsNotification != 0);
				cntSettings++;

				int rnOldvalue = 0;
				int rnvalue = 0;

				m_sql.GetPreferencesVar("ActiveTimerPlan", rnOldvalue);
				rnvalue = atoi(request::findValue(&req, "ActiveTimerPlan").c_str());
				if (rnOldvalue != rnvalue)
				{
					m_sql.UpdatePreferencesVar("ActiveTimerPlan", rnvalue);
					m_sql.m_ActiveTimerPlan = rnvalue;
					m_mainworker.m_scheduler.ReloadSchedules();
				}
				cntSettings++;

				rnOldvalue = 0;
				m_sql.GetPreferencesVar("NotificationSensorInterval", rnOldvalue);
				rnvalue = atoi(request::findValue(&req, "NotificationSensorInterval").c_str());
				if (rnOldvalue != rnvalue)
				{
					m_sql.UpdatePreferencesVar("NotificationSensorInterval", rnvalue);
					m_notifications.ReloadNotifications();
				}
				cntSettings++;

				rnOldvalue = 0;
				m_sql.GetPreferencesVar("NotificationSwitchInterval", rnOldvalue);
				rnvalue = atoi(request::findValue(&req, "NotificationSwitchInterval").c_str());
				if (rnOldvalue != rnvalue)
				{
					m_sql.UpdatePreferencesVar("NotificationSwitchInterval", rnvalue);
					m_notifications.ReloadNotifications();
				}
				cntSettings++;

				rnOldvalue = 0;
				m_sql.GetPreferencesVar("EnableEventScriptSystem", rnOldvalue);
				std::string EnableEventScriptSystem = request::findValue(&req, "EnableEventScriptSystem");
				int iEnableEventScriptSystem = (EnableEventScriptSystem == "on" ? 1 : 0);
				m_sql.UpdatePreferencesVar("EnableEventScriptSystem", iEnableEventScriptSystem);
				m_sql.m_bEnableEventSystem = (iEnableEventScriptSystem == 1);
				if (iEnableEventScriptSystem != rnOldvalue)
				{
					m_mainworker.m_eventsystem.SetEnabled(m_sql.m_bEnableEventSystem);
					m_mainworker.m_eventsystem.StartEventSystem();
				}
				cntSettings++;

				std::string EnableEventSystemFullURLLog = request::findValue(&req, "EventSystemLogFullURL");
				m_sql.m_bEnableEventSystemFullURLLog = EnableEventSystemFullURLLog == "on" ? true : false;
				m_sql.UpdatePreferencesVar("EventSystemLogFullURL", (int)m_sql.m_bEnableEventSystemFullURLLog);
				cntSettings++;

				rnOldvalue = 0;
				m_sql.GetPreferencesVar("DisableDzVentsSystem", rnOldvalue);
				std::string DisableDzVentsSystem = request::findValue(&req, "DisableDzVentsSystem");
				int iDisableDzVentsSystem = (DisableDzVentsSystem == "on" ? 0 : 1);
				m_sql.UpdatePreferencesVar("DisableDzVentsSystem", iDisableDzVentsSystem);
				m_sql.m_bDisableDzVentsSystem = (iDisableDzVentsSystem == 1);
				if (m_sql.m_bEnableEventSystem && !iDisableDzVentsSystem && iDisableDzVentsSystem != rnOldvalue)
				{
					m_mainworker.m_eventsystem.LoadEvents();
					m_mainworker.m_eventsystem.GetCurrentStates();
				}
				cntSettings++;
				m_sql.UpdatePreferencesVar("DzVentsLogLevel", atoi(request::findValue(&req, "DzVentsLogLevel").c_str()));
				cntSettings++;

				rnOldvalue = 0;
				m_sql.GetPreferencesVar("RemoteSharedPort", rnOldvalue);
				m_sql.UpdatePreferencesVar("RemoteSharedPort", atoi(request::findValue(&req, "RemoteSharedPort").c_str()));
				m_sql.GetPreferencesVar("RemoteSharedPort", rnvalue);

				if (rnvalue != rnOldvalue)
				{
					m_mainworker.m_sharedserver.StopServer();
					if (rnvalue != 0)
					{
						char szPort[100];
						sprintf(szPort, "%d", rnvalue);
						m_mainworker.m_sharedserver.StartServer("::", szPort);
						m_mainworker.LoadSharedUsers();
					}
				}
				cntSettings++;

				rnOldvalue = 0;
				m_sql.GetPreferencesVar("RandomTimerFrame", rnOldvalue);
				rnvalue = atoi(request::findValue(&req, "RandomSpread").c_str());
				if (rnOldvalue != rnvalue)
				{
					m_sql.UpdatePreferencesVar("RandomTimerFrame", rnvalue);
					m_mainworker.m_scheduler.ReloadSchedules();
				}
				cntSettings++;

				rnOldvalue = 0;
				int batterylowlevel = atoi(request::findValue(&req, "BatterLowLevel").c_str());
				if (batterylowlevel > 100)
					batterylowlevel = 100;
				m_sql.GetPreferencesVar("BatteryLowNotification", rnOldvalue);
				m_sql.UpdatePreferencesVar("BatteryLowNotification", batterylowlevel);
				if ((rnOldvalue != batterylowlevel) && (batterylowlevel != 0))
					m_sql.CheckBatteryLow();
				cntSettings++;

				/* Update the Theme */

				std::string SelectedTheme = request::findValue(&req, "Themes");
				m_sql.UpdatePreferencesVar("WebTheme", SelectedTheme);
				m_pWebEm->SetWebTheme(SelectedTheme);
				cntSettings++;

				//Update the Max kWh value
				rnvalue = 6000;
				if (m_sql.GetPreferencesVar("MaxElectricPower", rnvalue))
				{
					if (rnvalue < 1)
						rnvalue = 6000;
					m_sql.m_max_kwh_usage = rnvalue;
				}

				/* To wrap up everything */
				m_notifications.ConfigFromGetvars(req, true);
				m_notifications.LoadConfig();

#ifdef ENABLE_PYTHON
				// Signal plugins to update Settings dictionary
				PluginLoadConfig();
#endif
				root["status"] = "OK";
			}
			catch (const std::exception& e)
			{
				std::stringstream errmsg;
				errmsg << "Error occured during processing of POSTed settings (" << e.what() << ") after processing " << cntSettings << " settings!";
				root["errmsg"] = errmsg.str();
				_log.Log(LOG_ERROR, errmsg.str());
			}
			catch (...)
			{
				std::stringstream errmsg;
				errmsg << "Error occured during processing of POSTed settings after processing " << cntSettings << " settings!";
				root["errmsg"] = errmsg.str();
				_log.Log(LOG_ERROR, errmsg.str());
			}
			std::string msg = "Processed " + std::to_string(cntSettings) + " settings!";
			root["message"] = msg;
		}

		void CWebServer::RestoreDatabase(WebEmSession& session, const request& req, std::string& redirect_uri)
		{
			redirect_uri = "/index.html";
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string dbasefile = request::findValue(&req, "dbasefile");
			if (dbasefile.empty())
			{
				return;
			}

			m_mainworker.StopDomoticzHardware();

			m_sql.RestoreDatabase(dbasefile);
			m_mainworker.AddAllDomoticzHardware();
		}

		struct _tHardwareListInt
		{
			std::string Name;
			int HardwareTypeVal;
			std::string HardwareType;
			bool Enabled;
			std::string Mode1; // Used to flag DimmerType as relative for some old LimitLessLight type bulbs
			std::string Mode2; // Used to flag DimmerType as relative for some old LimitLessLight type bulbs
		};

		void CWebServer::GetJSonDevices(Json::Value& root, const std::string& rused, const std::string& rfilter, const std::string& order, const std::string& rowid, const std::string& planID,
			const std::string& floorID, const bool bDisplayHidden, const bool bDisplayDisabled, const bool bFetchFavorites, const time_t LastUpdate,
			const std::string& username, const std::string& hardwareid)
		{
			std::vector<std::vector<std::string>> result;

			time_t now = mytime(nullptr);
			struct tm tm1;
			localtime_r(&now, &tm1);
			struct tm tLastUpdate;
			localtime_r(&now, &tLastUpdate);

			const time_t iLastUpdate = LastUpdate - 1;

			int SensorTimeOut = 60;
			m_sql.GetPreferencesVar("SensorTimeout", SensorTimeOut);

			// Get All Hardware ID's/Names, need them later
			std::map<int, _tHardwareListInt> _hardwareNames;
			result = m_sql.safe_query("SELECT ID, Name, Enabled, Type, Mode1, Mode2 FROM Hardware");
			if (!result.empty())
			{
				for (const auto& sd : result)
				{
					_tHardwareListInt tlist;
					int ID = atoi(sd[0].c_str());
					tlist.Name = sd[1];
					tlist.Enabled = (atoi(sd[2].c_str()) != 0);
					tlist.HardwareTypeVal = atoi(sd[3].c_str());
#ifndef ENABLE_PYTHON
					tlist.HardwareType = Hardware_Type_Desc(tlist.HardwareTypeVal);
#else
					if (tlist.HardwareTypeVal != HTYPE_PythonPlugin)
					{
						tlist.HardwareType = Hardware_Type_Desc(tlist.HardwareTypeVal);
					}
					else
					{
						tlist.HardwareType = PluginHardwareDesc(ID);
					}
#endif
					tlist.Mode1 = sd[4];
					tlist.Mode2 = sd[5];
					_hardwareNames[ID] = tlist;
				}
			}

			root["ActTime"] = static_cast<int>(now);

			char szTmp[300];

			if (!m_mainworker.m_LastSunriseSet.empty())
			{
				std::vector<std::string> strarray;
				StringSplit(m_mainworker.m_LastSunriseSet, ";", strarray);
				if (strarray.size() == 10)
				{
					// strftime(szTmp, 80, "%b %d %Y %X", &tm1);
					strftime(szTmp, 80, "%Y-%m-%d %X", &tm1);
					root["ServerTime"] = szTmp;
					root["Sunrise"] = strarray[0];
					root["Sunset"] = strarray[1];
					root["SunAtSouth"] = strarray[2];
					root["CivTwilightStart"] = strarray[3];
					root["CivTwilightEnd"] = strarray[4];
					root["NautTwilightStart"] = strarray[5];
					root["NautTwilightEnd"] = strarray[6];
					root["AstrTwilightStart"] = strarray[7];
					root["AstrTwilightEnd"] = strarray[8];
					root["DayLength"] = strarray[9];
				}
			}

			char szOrderBy[50];
			std::string szQuery;
			bool isAlpha = true;
			const std::string orderBy = order;
			for (char i : orderBy)
			{
				if (!isalpha(i))
				{
					isAlpha = false;
				}
			}
			if (order.empty() || (!isAlpha))
			{
				strcpy(szOrderBy, "A.[Order],A.LastUpdate DESC");
			}
			else
			{
				sprintf(szOrderBy, "A.[Order],A.%s ASC", order.c_str());
			}

			unsigned char tempsign = m_sql.m_tempsign[0];

			bool bHaveUser = false;
			int iUser = -1;
			unsigned int totUserDevices = 0;
			bool bShowScenes = true;
			bHaveUser = (!username.empty());
			if (bHaveUser)
			{
				iUser = FindUser(username.c_str());
				if (iUser != -1)
				{
					
					if (m_users[iUser].TotSensors > 0)
					{
						bool bSkipSelectedDevices = false;
						if (m_users[iUser].userrights == URIGHTS_ADMIN)
						{
							bSkipSelectedDevices = (rused == "all");
						}
						if (!bSkipSelectedDevices)
						{
							result = m_sql.safe_query("SELECT COUNT(*) FROM SharedDevices WHERE (SharedUserID == %lu)", m_users[iUser].ID);
							if (!result.empty())
							{
								totUserDevices = (unsigned int)std::stoi(result[0][0]);
							}
						}
					}
					bShowScenes = (m_users[iUser].ActiveTabs & (1 << 1)) != 0;
				}
			}

			std::set<std::string> _HiddenDevices;
			bool bAllowDeviceToBeHidden = false;

			int ii = 0;
			if (rfilter == "all")
			{
				if ((bShowScenes) && ((rused == "all") || (rused == "true")))
				{
					// add scenes
					if (!rowid.empty())
						result = m_sql.safe_query("SELECT A.ID, A.Name, A.nValue, A.LastUpdate, A.Favorite, A.SceneType,"
							" A.Protected, B.XOffset, B.YOffset, B.PlanID, A.Description"
							" FROM Scenes as A"
							" LEFT OUTER JOIN DeviceToPlansMap as B ON (B.DeviceRowID==a.ID) AND (B.DevSceneType==1)"
							" WHERE (A.ID=='%q')",
							rowid.c_str());
					else if ((!planID.empty()) && (planID != "0"))
						result = m_sql.safe_query("SELECT A.ID, A.Name, A.nValue, A.LastUpdate, A.Favorite, A.SceneType,"
							" A.Protected, B.XOffset, B.YOffset, B.PlanID, A.Description"
							" FROM Scenes as A, DeviceToPlansMap as B WHERE (B.PlanID=='%q')"
							" AND (B.DeviceRowID==a.ID) AND (B.DevSceneType==1) ORDER BY B.[Order]",
							planID.c_str());
					else if ((!floorID.empty()) && (floorID != "0"))
						result = m_sql.safe_query("SELECT A.ID, A.Name, A.nValue, A.LastUpdate, A.Favorite, A.SceneType,"
							" A.Protected, B.XOffset, B.YOffset, B.PlanID, A.Description"
							" FROM Scenes as A, DeviceToPlansMap as B, Plans as C"
							" WHERE (C.FloorplanID=='%q') AND (C.ID==B.PlanID) AND (B.DeviceRowID==a.ID)"
							" AND (B.DevSceneType==1) ORDER BY B.[Order]",
							floorID.c_str());
					else
					{
						szQuery = ("SELECT A.ID, A.Name, A.nValue, A.LastUpdate, A.Favorite, A.SceneType,"
							" A.Protected, B.XOffset, B.YOffset, B.PlanID, A.Description"
							" FROM Scenes as A"
							" LEFT OUTER JOIN DeviceToPlansMap as B ON (B.DeviceRowID==a.ID) AND (B.DevSceneType==1)"
							" ORDER BY ");
						szQuery += szOrderBy;
						result = m_sql.safe_query(szQuery.c_str(), order.c_str());
					}

					if (!result.empty())
					{
						for (const auto& sd : result)
						{
							unsigned char favorite = atoi(sd[4].c_str());
							// Check if we only want favorite devices
							if ((bFetchFavorites) && (!favorite))
								continue;

							std::string sLastUpdate = sd[3];

							if (iLastUpdate != 0)
							{
								time_t cLastUpdate;
								ParseSQLdatetime(cLastUpdate, tLastUpdate, sLastUpdate, tm1.tm_isdst);
								if (cLastUpdate <= iLastUpdate)
									continue;
							}

							int nValue = atoi(sd[2].c_str());

							unsigned char scenetype = atoi(sd[5].c_str());
							int iProtected = atoi(sd[6].c_str());

							std::string sSceneName = sd[1];
							if (!bDisplayHidden && sSceneName[0] == '$')
							{
								continue;
							}

							if (scenetype == 0)
							{
								root["result"][ii]["Type"] = "Scene";
								root["result"][ii]["TypeImg"] = "scene";
								root["result"][ii]["Image"] = "Push";
							}
							else
							{
								root["result"][ii]["Type"] = "Group";
								root["result"][ii]["TypeImg"] = "group";
							}

							// has this scene/group already been seen, now with different plan?
							// assume results are ordered such that same device is adjacent
							// if the idx and the Type are equal (type to prevent matching against Scene with same idx)
							std::string thisIdx = sd[0];

							if ((ii > 0) && thisIdx == root["result"][ii - 1]["idx"].asString())
							{
								std::string typeOfThisOne = root["result"][ii]["Type"].asString();
								if (typeOfThisOne == root["result"][ii - 1]["Type"].asString())
								{
									root["result"][ii - 1]["PlanIDs"].append(atoi(sd[9].c_str()));
									continue;
								}
							}

							root["result"][ii]["idx"] = sd[0];
							root["result"][ii]["Name"] = sSceneName;
							root["result"][ii]["Description"] = sd[10];
							root["result"][ii]["Favorite"] = favorite;
							root["result"][ii]["Protected"] = (iProtected != 0);
							root["result"][ii]["LastUpdate"] = sLastUpdate;
							root["result"][ii]["PlanID"] = sd[9].c_str();
							Json::Value jsonArray;
							jsonArray.append(atoi(sd[9].c_str()));
							root["result"][ii]["PlanIDs"] = jsonArray;

							if (nValue == 0)
								root["result"][ii]["Status"] = "Off";
							else if (nValue == 1)
								root["result"][ii]["Status"] = "On";
							else
								root["result"][ii]["Status"] = "Mixed";
							root["result"][ii]["Data"] = root["result"][ii]["Status"];
							uint64_t camIDX = m_mainworker.m_cameras.IsDevSceneInCamera(1, sd[0]);
							root["result"][ii]["UsedByCamera"] = (camIDX != 0) ? true : false;
							if (camIDX != 0)
							{
								std::stringstream scidx;
								scidx << camIDX;
								root["result"][ii]["CameraIdx"] = scidx.str();
								root["result"][ii]["CameraAspect"] = m_mainworker.m_cameras.GetCameraAspectRatio(scidx.str());
							}
							root["result"][ii]["XOffset"] = atoi(sd[7].c_str());
							root["result"][ii]["YOffset"] = atoi(sd[8].c_str());
							ii++;
						}
					}
				}
			}

			char szData[320];
			if (totUserDevices == 0)
			{
				// All
				if (!rowid.empty())
				{
					//_log.Log(LOG_STATUS, "Getting device with id: %s", rowid.c_str());
					result = m_sql.safe_query("SELECT A.ID, A.DeviceID, A.Unit, A.Name, A.Used, A.Type, A.SubType,"
						" A.SignalLevel, A.BatteryLevel, A.nValue, A.sValue,"
						" A.LastUpdate, A.Favorite, A.SwitchType, A.HardwareID,"
						" A.AddjValue, A.AddjMulti, A.AddjValue2, A.AddjMulti2,"
						" A.LastLevel, A.CustomImage, A.StrParam1, A.StrParam2,"
						" A.Protected, IFNULL(B.XOffset,0), IFNULL(B.YOffset,0), IFNULL(B.PlanID,0), A.Description,"
						" A.Options, A.Color "
						"FROM DeviceStatus A LEFT OUTER JOIN DeviceToPlansMap as B ON (B.DeviceRowID==a.ID) "
						"WHERE (A.ID=='%q')",
						rowid.c_str());
				}
				else if ((!planID.empty()) && (planID != "0"))
					result = m_sql.safe_query("SELECT A.ID, A.DeviceID, A.Unit, A.Name, A.Used,"
						" A.Type, A.SubType, A.SignalLevel, A.BatteryLevel,"
						" A.nValue, A.sValue, A.LastUpdate, A.Favorite,"
						" A.SwitchType, A.HardwareID, A.AddjValue,"
						" A.AddjMulti, A.AddjValue2, A.AddjMulti2,"
						" A.LastLevel, A.CustomImage, A.StrParam1,"
						" A.StrParam2, A.Protected, B.XOffset, B.YOffset,"
						" B.PlanID, A.Description,"
						" A.Options, A.Color "
						"FROM DeviceStatus as A, DeviceToPlansMap as B "
						"WHERE (B.PlanID=='%q') AND (B.DeviceRowID==a.ID)"
						" AND (B.DevSceneType==0) ORDER BY B.[Order]",
						planID.c_str());
				else if ((!floorID.empty()) && (floorID != "0"))
					result = m_sql.safe_query("SELECT A.ID, A.DeviceID, A.Unit, A.Name, A.Used,"
						" A.Type, A.SubType, A.SignalLevel, A.BatteryLevel,"
						" A.nValue, A.sValue, A.LastUpdate, A.Favorite,"
						" A.SwitchType, A.HardwareID, A.AddjValue,"
						" A.AddjMulti, A.AddjValue2, A.AddjMulti2,"
						" A.LastLevel, A.CustomImage, A.StrParam1,"
						" A.StrParam2, A.Protected, B.XOffset, B.YOffset,"
						" B.PlanID, A.Description,"
						" A.Options, A.Color "
						"FROM DeviceStatus as A, DeviceToPlansMap as B,"
						" Plans as C "
						"WHERE (C.FloorplanID=='%q') AND (C.ID==B.PlanID)"
						" AND (B.DeviceRowID==a.ID) AND (B.DevSceneType==0) "
						"ORDER BY B.[Order]",
						floorID.c_str());
				else
				{
					if (!bDisplayHidden)
					{
						// Build a list of Hidden Devices
						result = m_sql.safe_query("SELECT ID FROM Plans WHERE (Name=='$Hidden Devices')");
						if (!result.empty())
						{
							std::string pID = result[0][0];
							result = m_sql.safe_query("SELECT DeviceRowID FROM DeviceToPlansMap WHERE (PlanID=='%q') AND (DevSceneType==0)", pID.c_str());
							if (!result.empty())
							{
								for (const auto& r : result)
								{
									_HiddenDevices.insert(r[0]);
								}
							}
						}
						bAllowDeviceToBeHidden = true;
					}

					if (order.empty() || (!isAlpha))
						strcpy(szOrderBy, "A.[Order],A.LastUpdate DESC");
					else
					{
						sprintf(szOrderBy, "A.[Order],A.%s ASC", order.c_str());
					}
					//_log.Log(LOG_STATUS, "Getting all devices: order by %s ", szOrderBy);
					if (!hardwareid.empty())
					{
						szQuery = ("SELECT A.ID, A.DeviceID, A.Unit, A.Name, A.Used,A.Type, A.SubType,"
							" A.SignalLevel, A.BatteryLevel, A.nValue, A.sValue,"
							" A.LastUpdate, A.Favorite, A.SwitchType, A.HardwareID,"
							" A.AddjValue, A.AddjMulti, A.AddjValue2, A.AddjMulti2,"
							" A.LastLevel, A.CustomImage, A.StrParam1, A.StrParam2,"
							" A.Protected, IFNULL(B.XOffset,0), IFNULL(B.YOffset,0), IFNULL(B.PlanID,0), A.Description,"
							" A.Options, A.Color "
							"FROM DeviceStatus as A LEFT OUTER JOIN DeviceToPlansMap as B "
							"ON (B.DeviceRowID==a.ID) AND (B.DevSceneType==0) "
							"WHERE (A.HardwareID == %q) "
							"ORDER BY ");
						szQuery += szOrderBy;
						result = m_sql.safe_query(szQuery.c_str(), hardwareid.c_str(), order.c_str());
					}
					else
					{
						szQuery = ("SELECT A.ID, A.DeviceID, A.Unit, A.Name, A.Used,A.Type, A.SubType,"
							" A.SignalLevel, A.BatteryLevel, A.nValue, A.sValue,"
							" A.LastUpdate, A.Favorite, A.SwitchType, A.HardwareID,"
							" A.AddjValue, A.AddjMulti, A.AddjValue2, A.AddjMulti2,"
							" A.LastLevel, A.CustomImage, A.StrParam1, A.StrParam2,"
							" A.Protected, IFNULL(B.XOffset,0), IFNULL(B.YOffset,0), IFNULL(B.PlanID,0), A.Description,"
							" A.Options, A.Color "
							"FROM DeviceStatus as A LEFT OUTER JOIN DeviceToPlansMap as B "
							"ON (B.DeviceRowID==a.ID) AND (B.DevSceneType==0) "
							"ORDER BY ");
						szQuery += szOrderBy;
						result = m_sql.safe_query(szQuery.c_str(), order.c_str());
					}
				}
			}
			else
			{
				if (iUser == -1)
				{
					return;
				}
				// Specific devices
				if (!rowid.empty())
				{
					//_log.Log(LOG_STATUS, "Getting device with id: %s for user %lu", rowid.c_str(), m_users[iUser].ID);
					result = m_sql.safe_query("SELECT A.ID, A.DeviceID, A.Unit, A.Name, A.Used,"
						" A.Type, A.SubType, A.SignalLevel, A.BatteryLevel,"
						" A.nValue, A.sValue, A.LastUpdate, B.Favorite,"
						" A.SwitchType, A.HardwareID, A.AddjValue,"
						" A.AddjMulti, A.AddjValue2, A.AddjMulti2,"
						" A.LastLevel, A.CustomImage, A.StrParam1,"
						" A.StrParam2, A.Protected, 0 as XOffset,"
						" 0 as YOffset, 0 as PlanID, A.Description,"
						" A.Options, A.Color "
						"FROM DeviceStatus as A, SharedDevices as B "
						"WHERE (B.DeviceRowID==a.ID)"
						" AND (B.SharedUserID==%lu) AND (A.ID=='%q')",
						m_users[iUser].ID, rowid.c_str());
				}
				else if ((!planID.empty()) && (planID != "0"))
					result = m_sql.safe_query("SELECT A.ID, A.DeviceID, A.Unit, A.Name, A.Used,"
						" A.Type, A.SubType, A.SignalLevel, A.BatteryLevel,"
						" A.nValue, A.sValue, A.LastUpdate, B.Favorite,"
						" A.SwitchType, A.HardwareID, A.AddjValue,"
						" A.AddjMulti, A.AddjValue2, A.AddjMulti2,"
						" A.LastLevel, A.CustomImage, A.StrParam1,"
						" A.StrParam2, A.Protected, C.XOffset,"
						" C.YOffset, C.PlanID, A.Description,"
						" A.Options, A.Color "
						"FROM DeviceStatus as A, SharedDevices as B,"
						" DeviceToPlansMap as C "
						"WHERE (C.PlanID=='%q') AND (C.DeviceRowID==a.ID)"
						" AND (B.DeviceRowID==a.ID) "
						"AND (B.SharedUserID==%lu) ORDER BY C.[Order]",
						planID.c_str(), m_users[iUser].ID);
				else if ((!floorID.empty()) && (floorID != "0"))
					result = m_sql.safe_query("SELECT A.ID, A.DeviceID, A.Unit, A.Name, A.Used,"
						" A.Type, A.SubType, A.SignalLevel, A.BatteryLevel,"
						" A.nValue, A.sValue, A.LastUpdate, B.Favorite,"
						" A.SwitchType, A.HardwareID, A.AddjValue,"
						" A.AddjMulti, A.AddjValue2, A.AddjMulti2,"
						" A.LastLevel, A.CustomImage, A.StrParam1,"
						" A.StrParam2, A.Protected, C.XOffset, C.YOffset,"
						" C.PlanID, A.Description,"
						" A.Options, A.Color "
						"FROM DeviceStatus as A, SharedDevices as B,"
						" DeviceToPlansMap as C, Plans as D "
						"WHERE (D.FloorplanID=='%q') AND (D.ID==C.PlanID)"
						" AND (C.DeviceRowID==a.ID) AND (B.DeviceRowID==a.ID)"
						" AND (B.SharedUserID==%lu) ORDER BY C.[Order]",
						floorID.c_str(), m_users[iUser].ID);
				else
				{
					if (!bDisplayHidden)
					{
						// Build a list of Hidden Devices
						result = m_sql.safe_query("SELECT ID FROM Plans WHERE (Name=='$Hidden Devices')");
						if (!result.empty())
						{
							std::string pID = result[0][0];
							result = m_sql.safe_query("SELECT DeviceRowID FROM DeviceToPlansMap WHERE (PlanID=='%q')  AND (DevSceneType==0)", pID.c_str());
							if (!result.empty())
							{
								for (const auto& r : result)
								{
									_HiddenDevices.insert(r[0]);
								}
							}
						}
						bAllowDeviceToBeHidden = true;
					}

					if (order.empty() || (!isAlpha))
					{
						strcpy(szOrderBy, "B.[Order],A.LastUpdate DESC");
					}
					else
					{
						sprintf(szOrderBy, "B.[Order],A.%s ASC", order.c_str());
					}
					// _log.Log(LOG_STATUS, "Getting all devices for user %lu", m_users[iUser].ID);
					szQuery = ("SELECT A.ID, A.DeviceID, A.Unit, A.Name, A.Used,"
						" A.Type, A.SubType, A.SignalLevel, A.BatteryLevel,"
						" A.nValue, A.sValue, A.LastUpdate, B.Favorite,"
						" A.SwitchType, A.HardwareID, A.AddjValue,"
						" A.AddjMulti, A.AddjValue2, A.AddjMulti2,"
						" A.LastLevel, A.CustomImage, A.StrParam1,"
						" A.StrParam2, A.Protected, IFNULL(C.XOffset,0),"
						" IFNULL(C.YOffset,0), IFNULL(C.PlanID,0), A.Description,"
						" A.Options, A.Color "
						"FROM DeviceStatus as A, SharedDevices as B "
						"LEFT OUTER JOIN DeviceToPlansMap as C  ON (C.DeviceRowID==A.ID)"
						"WHERE (B.DeviceRowID==A.ID)"
						" AND (B.SharedUserID==%lu) ORDER BY ");
					szQuery += szOrderBy;
					result = m_sql.safe_query(szQuery.c_str(), m_users[iUser].ID, order.c_str());
				}
			}

			if (result.empty())
				return;

			for (const auto& sd : result)
			{
				try
				{
					unsigned char favorite = atoi(sd[12].c_str());
					bool bIsInPlan = !planID.empty() && (planID != "0");

					// Check if we only want favorite devices
					if (!bIsInPlan)
					{
						if ((bFetchFavorites) && (!favorite))
							continue;
					}

					std::string sDeviceName = sd[3];

					if (!bDisplayHidden)
					{
						if (_HiddenDevices.find(sd[0]) != _HiddenDevices.end())
							continue;
						if (sDeviceName[0] == '$')
						{
							if (bAllowDeviceToBeHidden)
								continue;
							if (!planID.empty())
								sDeviceName = sDeviceName.substr(1);
						}
					}
					int hardwareID = atoi(sd[14].c_str());
					auto hItt = _hardwareNames.find(hardwareID);
					bool bIsHardwareDisabled = true;
					if (hItt != _hardwareNames.end())
					{
						// ignore sensors where the hardware is disabled
						if ((!bDisplayDisabled) && (!(*hItt).second.Enabled))
							continue;
						bIsHardwareDisabled = !(*hItt).second.Enabled;
					}

					unsigned int dType = atoi(sd[5].c_str());
					unsigned int dSubType = atoi(sd[6].c_str());
					unsigned int used = atoi(sd[4].c_str());
					int nValue = atoi(sd[9].c_str());
					std::string sValue = sd[10];
					std::string sLastUpdate = sd[11];
					if (sLastUpdate.size() > 19)
						sLastUpdate = sLastUpdate.substr(0, 19);

					if (iLastUpdate != 0)
					{
						time_t cLastUpdate;
						ParseSQLdatetime(cLastUpdate, tLastUpdate, sLastUpdate, tm1.tm_isdst);
						if (cLastUpdate <= iLastUpdate)
							continue;
					}

					_eSwitchType switchtype = (_eSwitchType)atoi(sd[13].c_str());
					_eMeterType metertype = (_eMeterType)switchtype;
					double AddjValue = atof(sd[15].c_str());
					double AddjMulti = atof(sd[16].c_str());
					double AddjValue2 = atof(sd[17].c_str());
					double AddjMulti2 = atof(sd[18].c_str());
					int LastLevel = atoi(sd[19].c_str());
					int CustomImage = atoi(sd[20].c_str());
					std::string strParam1 = base64_encode(sd[21]);
					std::string strParam2 = base64_encode(sd[22]);
					int iProtected = atoi(sd[23].c_str());

					std::string Description = sd[27];
					std::string sOptions = sd[28];
					std::string sColor = sd[29];
					std::map<std::string, std::string> options = m_sql.BuildDeviceOptions(sOptions);

					struct tm ntime;
					time_t checktime;
					ParseSQLdatetime(checktime, ntime, sLastUpdate, tm1.tm_isdst);
					bool bHaveTimeout = (now - checktime >= SensorTimeOut * 60);

					if (dType == pTypeTEMP_RAIN)
						continue; // dont want you for now

					if ((rused == "true") && (!used))
						continue;

					if ((rused == "false") && (used))
						continue;
					if (!rfilter.empty())
					{
						if (rfilter == "light")
						{
							if ((dType != pTypeLighting1) && (dType != pTypeLighting2) && (dType != pTypeLighting3) && (dType != pTypeLighting4) &&
								(dType != pTypeLighting5) && (dType != pTypeLighting6) && (dType != pTypeFan) && (dType != pTypeColorSwitch) && (dType != pTypeSecurity1) &&
								(dType != pTypeSecurity2) && (dType != pTypeEvohome) && (dType != pTypeEvohomeRelay) && (dType != pTypeCurtain) && (dType != pTypeBlinds) &&
								(dType != pTypeRFY) && (dType != pTypeChime) && (dType != pTypeThermostat2) && (dType != pTypeThermostat3) && (dType != pTypeThermostat4) &&
								(dType != pTypeRemote) && (dType != pTypeGeneralSwitch) && (dType != pTypeHomeConfort) && (dType != pTypeChime) && (dType != pTypeFS20) &&
								(!((dType == pTypeRego6XXValue) && (dSubType == sTypeRego6XXStatus))) &&
								(!((dType == pTypeRadiator1) && (dSubType == sTypeSmartwaresSwitchRadiator))) && (dType != pTypeHunter))
								continue;
						}
						else if (rfilter == "temp")
						{
							if ((dType != pTypeTEMP) && (dType != pTypeHUM) && (dType != pTypeTEMP_HUM) && (dType != pTypeTEMP_HUM_BARO) && (dType != pTypeTEMP_BARO) &&
								(dType != pTypeEvohomeZone) && (dType != pTypeEvohomeWater) && (!((dType == pTypeWIND) && (dSubType == sTypeWIND4))) &&
								(!((dType == pTypeUV) && (dSubType == sTypeUV3))) && (!((dType == pTypeGeneral) && (dSubType == sTypeSystemTemp))) &&
								(dType != pTypeThermostat1) && (!((dType == pTypeRFXSensor) && (dSubType == sTypeRFXSensorTemp))) && (dType != pTypeRego6XXTemp))
								continue;
						}
						else if (rfilter == "weather")
						{
							if ((dType != pTypeWIND) && (dType != pTypeRAIN) && (dType != pTypeTEMP_HUM_BARO) && (dType != pTypeTEMP_BARO) && (dType != pTypeUV) &&
								(!((dType == pTypeGeneral) && (dSubType == sTypeVisibility))) && (!((dType == pTypeGeneral) && (dSubType == sTypeBaro))) &&
								(!((dType == pTypeGeneral) && (dSubType == sTypeSolarRadiation))))
								continue;
						}
						else if (rfilter == "utility")
						{
							if ((dType != pTypeRFXMeter) && (!((dType == pTypeRFXSensor) && (dSubType == sTypeRFXSensorAD))) &&
								(!((dType == pTypeRFXSensor) && (dSubType == sTypeRFXSensorVolt))) && (!((dType == pTypeGeneral) && (dSubType == sTypeVoltage))) &&
								(!((dType == pTypeGeneral) && (dSubType == sTypeCurrent))) && (!((dType == pTypeGeneral) && (dSubType == sTypeTextStatus))) &&
								(!((dType == pTypeGeneral) && (dSubType == sTypeAlert))) && (!((dType == pTypeGeneral) && (dSubType == sTypePressure))) &&
								(!((dType == pTypeGeneral) && (dSubType == sTypeSoilMoisture))) && (!((dType == pTypeGeneral) && (dSubType == sTypeLeafWetness))) &&
								(!((dType == pTypeGeneral) && (dSubType == sTypePercentage))) && (!((dType == pTypeGeneral) && (dSubType == sTypeWaterflow))) &&
								(!((dType == pTypeGeneral) && (dSubType == sTypeCustom))) && (!((dType == pTypeGeneral) && (dSubType == sTypeFan))) &&
								(!((dType == pTypeGeneral) && (dSubType == sTypeDistance))) && (!((dType == pTypeGeneral) && (dSubType == sTypeCounterIncremental))) &&
								(!((dType == pTypeGeneral) && (dSubType == sTypeManagedCounter))) && (!((dType == pTypeGeneral) && (dSubType == sTypeKwh))) &&
								(dType != pTypeCURRENT) && (dType != pTypeCURRENTENERGY) && (dType != pTypeENERGY) && (dType != pTypePOWER) && (dType != pTypeP1Power) &&
								(dType != pTypeP1Gas) && (dType != pTypeYouLess) && (dType != pTypeAirQuality) && (dType != pTypeLux) && (dType != pTypeUsage) &&
								(!((dType == pTypeRego6XXValue) && (dSubType == sTypeRego6XXCounter))) &&
								(!((dType == pTypeThermostat) && (dSubType == sTypeThermSetpoint))) && (dType != pTypeWEIGHT) &&
								(!((dType == pTypeRadiator1) && (dSubType == sTypeSmartwares))))
								continue;
						}
						else if (rfilter == "wind")
						{
							if ((dType != pTypeWIND))
								continue;
						}
						else if (rfilter == "rain")
						{
							if ((dType != pTypeRAIN))
								continue;
						}
						else if (rfilter == "uv")
						{
							if ((dType != pTypeUV))
								continue;
						}
						else if (rfilter == "baro")
						{
							if ((dType != pTypeTEMP_HUM_BARO) && (dType != pTypeTEMP_BARO))
								continue;
						}
					}

					// has this device already been seen, now with different plan?
					// assume results are ordered such that same device is adjacent
					// if the idx and the Type are equal (type to prevent matching against Scene with same idx)
					std::string thisIdx = sd[0];
					const int devIdx = atoi(thisIdx.c_str());

					if ((ii > 0) && thisIdx == root["result"][ii - 1]["idx"].asString())
					{
						std::string typeOfThisOne = RFX_Type_Desc(dType, 1);
						if (typeOfThisOne == root["result"][ii - 1]["Type"].asString())
						{
							root["result"][ii - 1]["PlanIDs"].append(atoi(sd[26].c_str()));
							continue;
						}
					}

					root["result"][ii]["HardwareID"] = hardwareID;
					if (_hardwareNames.find(hardwareID) == _hardwareNames.end())
					{
						root["result"][ii]["HardwareName"] = "Unknown?";
						root["result"][ii]["HardwareTypeVal"] = 0;
						root["result"][ii]["HardwareType"] = "Unknown?";
					}
					else
					{
						root["result"][ii]["HardwareName"] = _hardwareNames[hardwareID].Name;
						root["result"][ii]["HardwareTypeVal"] = _hardwareNames[hardwareID].HardwareTypeVal;
						root["result"][ii]["HardwareType"] = _hardwareNames[hardwareID].HardwareType;
					}
					root["result"][ii]["HardwareDisabled"] = bIsHardwareDisabled;

					root["result"][ii]["idx"] = sd[0];
					root["result"][ii]["Protected"] = (iProtected != 0);

					CDomoticzHardwareBase* pHardware = m_mainworker.GetHardware(hardwareID);
					if (pHardware != nullptr)
					{
						if (pHardware->HwdType == HTYPE_SolarEdgeAPI)
						{
							int seSensorTimeOut = 60 * 24 * 60;
							bHaveTimeout = (now - checktime >= seSensorTimeOut * 60);
						}
						else if (pHardware->HwdType == HTYPE_Wunderground)
						{
							CWunderground* pWHardware = dynamic_cast<CWunderground*>(pHardware);
							std::string forecast_url = pWHardware->GetForecastURL();
							if (!forecast_url.empty())
							{
								root["result"][ii]["forecast_url"] = base64_encode(forecast_url);
							}
						}
						else if (pHardware->HwdType == HTYPE_DarkSky)
						{
							CDarkSky* pWHardware = dynamic_cast<CDarkSky*>(pHardware);
							std::string forecast_url = pWHardware->GetForecastURL();
							if (!forecast_url.empty())
							{
								root["result"][ii]["forecast_url"] = base64_encode(forecast_url);
							}
						}
						else if (pHardware->HwdType == HTYPE_VisualCrossing)
						{
							CVisualCrossing* pWHardware = dynamic_cast<CVisualCrossing*>(pHardware);
							std::string forecast_url = pWHardware->GetForecastURL();
							if (!forecast_url.empty())
							{
								root["result"][ii]["forecast_url"] = base64_encode(forecast_url);
							}
						}
						else if (pHardware->HwdType == HTYPE_AccuWeather)
						{
							CAccuWeather* pWHardware = dynamic_cast<CAccuWeather*>(pHardware);
							std::string forecast_url = pWHardware->GetForecastURL();
							if (!forecast_url.empty())
							{
								root["result"][ii]["forecast_url"] = base64_encode(forecast_url);
							}
						}
						else if (pHardware->HwdType == HTYPE_OpenWeatherMap)
						{
							COpenWeatherMap* pWHardware = dynamic_cast<COpenWeatherMap*>(pHardware);
							std::string forecast_url = pWHardware->GetForecastURL();
							if (!forecast_url.empty())
							{
								root["result"][ii]["forecast_url"] = base64_encode(forecast_url);
							}
						}
						else if (pHardware->HwdType == HTYPE_BuienRadar)
						{
							CBuienRadar* pWHardware = dynamic_cast<CBuienRadar*>(pHardware);
							std::string forecast_url = pWHardware->GetForecastURL();
							if (!forecast_url.empty())
							{
								root["result"][ii]["forecast_url"] = base64_encode(forecast_url);
							}
						}
						else if (pHardware->HwdType == HTYPE_Meteorologisk)
						{
							CMeteorologisk* pWHardware = dynamic_cast<CMeteorologisk*>(pHardware);
							std::string forecast_url = pWHardware->GetForecastURL();
							if (!forecast_url.empty())
							{
								root["result"][ii]["forecast_url"] = base64_encode(forecast_url);
							}
						}
					}

					if ((pHardware != nullptr) && (pHardware->HwdType == HTYPE_PythonPlugin))
					{
						// Device ID special formatting should not be applied to Python plugins
						root["result"][ii]["ID"] = sd[1];
					}
					else
					{
						if ((dType == pTypeTEMP) || (dType == pTypeTEMP_BARO) || (dType == pTypeTEMP_HUM) || (dType == pTypeTEMP_HUM_BARO) || (dType == pTypeBARO) ||
							(dType == pTypeHUM) || (dType == pTypeWIND) || (dType == pTypeRAIN) || (dType == pTypeUV) || (dType == pTypeCURRENT) ||
							(dType == pTypeCURRENTENERGY) || (dType == pTypeENERGY) || (dType == pTypeRFXMeter) || (dType == pTypeAirQuality) || (dType == pTypeRFXSensor) ||
							(dType == pTypeP1Power) || (dType == pTypeP1Gas))
						{
							root["result"][ii]["ID"] = is_number(sd[1]) ? std_format("%04X", (unsigned int)atoi(sd[1].c_str())) : sd[1];
						}
						else
						{
							root["result"][ii]["ID"] = sd[1];
						}
					}

					root["result"][ii]["Unit"] = atoi(sd[2].c_str());
					root["result"][ii]["Type"] = RFX_Type_Desc(dType, 1);
					root["result"][ii]["SubType"] = RFX_Type_SubType_Desc(dType, dSubType);
					root["result"][ii]["TypeImg"] = RFX_Type_Desc(dType, 2);
					root["result"][ii]["Name"] = sDeviceName;
					root["result"][ii]["Description"] = Description;
					root["result"][ii]["Used"] = used;
					root["result"][ii]["Favorite"] = favorite;

					int iSignalLevel = atoi(sd[7].c_str());
					if (iSignalLevel < 12)
						root["result"][ii]["SignalLevel"] = iSignalLevel;
					else
						root["result"][ii]["SignalLevel"] = "-";
					root["result"][ii]["BatteryLevel"] = atoi(sd[8].c_str());
					root["result"][ii]["LastUpdate"] = sLastUpdate;

					root["result"][ii]["CustomImage"] = CustomImage;

					if (CustomImage != 0)
					{
						auto ittIcon = m_custom_light_icons_lookup.find(CustomImage);
						if (ittIcon != m_custom_light_icons_lookup.end())
						{
							root["result"][ii]["CustomImage"] = CustomImage;
							root["result"][ii]["Image"] = m_custom_light_icons[ittIcon->second].RootFile;
						}
						else
						{
							CustomImage = 0;
							root["result"][ii]["CustomImage"] = CustomImage;
						}
					}

					root["result"][ii]["XOffset"] = sd[24].c_str();
					root["result"][ii]["YOffset"] = sd[25].c_str();
					root["result"][ii]["PlanID"] = sd[26].c_str();
					Json::Value jsonArray;
					jsonArray.append(atoi(sd[26].c_str()));
					root["result"][ii]["PlanIDs"] = jsonArray;
					root["result"][ii]["AddjValue"] = AddjValue;
					root["result"][ii]["AddjMulti"] = AddjMulti;
					root["result"][ii]["AddjValue2"] = AddjValue2;
					root["result"][ii]["AddjMulti2"] = AddjMulti2;

					std::stringstream s_data;
					s_data << int(nValue) << ", " << sValue;
					root["result"][ii]["Data"] = s_data.str();

					root["result"][ii]["Notifications"] = (m_notifications.HasNotifications(sd[0]) == true) ? "true" : "false";
					root["result"][ii]["ShowNotifications"] = true;

					bool bHasTimers = false;

					if (
						(dType == pTypeLighting1)
						|| (dType == pTypeLighting2)
						|| (dType == pTypeLighting3)
						|| (dType == pTypeLighting4)
						|| (dType == pTypeLighting5)
						|| (dType == pTypeLighting6)
						|| (dType == pTypeFan)
						|| (dType == pTypeColorSwitch)
						|| (dType == pTypeCurtain)
						|| (dType == pTypeBlinds)
						|| (dType == pTypeRFY)
						|| (dType == pTypeChime)
						|| (dType == pTypeThermostat2)
						|| (dType == pTypeThermostat3)
						|| (dType == pTypeThermostat4)
						|| (dType == pTypeRemote)
						|| (dType == pTypeGeneralSwitch)
						|| (dType == pTypeHomeConfort)
						|| (dType == pTypeFS20)
						|| ((dType == pTypeRadiator1) && (dSubType == sTypeSmartwaresSwitchRadiator))
						|| ((dType == pTypeRego6XXValue) && (dSubType == sTypeRego6XXStatus))
						|| (dType == pTypeHunter))
					{
						// add light details
						bHasTimers = m_sql.HasTimers(sd[0]);

						bHaveTimeout = false;
						root["result"][ii]["HaveTimeout"] = bHaveTimeout;

						std::string lstatus;
						int llevel = 0;
						bool bHaveDimmer = false;
						bool bHaveGroupCmd = false;
						int maxDimLevel = 0;

						GetLightStatus(dType, dSubType, switchtype, nValue, sValue, lstatus, llevel, bHaveDimmer, maxDimLevel, bHaveGroupCmd);

						root["result"][ii]["Status"] = lstatus;
						root["result"][ii]["StrParam1"] = strParam1;
						root["result"][ii]["StrParam2"] = strParam2;

						if (!CustomImage)
							root["result"][ii]["Image"] = "Light";

						if (switchtype == STYPE_Dimmer)
						{
							root["result"][ii]["Level"] = LastLevel;
							int iLevel = round((float(maxDimLevel) / 100.0F) * LastLevel);
							root["result"][ii]["LevelInt"] = iLevel;
							if ((dType == pTypeColorSwitch) || (dType == pTypeLighting5 && dSubType == sTypeTRC02) ||
								(dType == pTypeLighting5 && dSubType == sTypeTRC02_2) || (dType == pTypeGeneralSwitch && dSubType == sSwitchTypeTRC02) ||
								(dType == pTypeGeneralSwitch && dSubType == sSwitchTypeTRC02_2))
							{
								_tColor color(sColor);
								std::string jsonColor = color.toJSONString();
								root["result"][ii]["Color"] = jsonColor;
								llevel = LastLevel;
								if (lstatus == "Set Level" || lstatus == "Set Color")
								{
									sprintf(szTmp, "Set Level: %d %%", LastLevel);
									root["result"][ii]["Status"] = szTmp;
								}
							}
						}
						else
						{
							root["result"][ii]["Level"] = llevel;
							root["result"][ii]["LevelInt"] = atoi(sValue.c_str());
						}
						root["result"][ii]["HaveDimmer"] = bHaveDimmer;
						std::string DimmerType = "none";
						if (switchtype == STYPE_Dimmer)
						{
							DimmerType = "abs";
							if (_hardwareNames.find(hardwareID) != _hardwareNames.end())
							{
								// Milight V4/V5 bridges do not support absolute dimming for RGB or CW_WW lights
								if (_hardwareNames[hardwareID].HardwareTypeVal == HTYPE_LimitlessLights &&
									atoi(_hardwareNames[hardwareID].Mode2.c_str()) != CLimitLess::LBTYPE_V6 &&
									(atoi(_hardwareNames[hardwareID].Mode1.c_str()) == sTypeColor_RGB ||
										atoi(_hardwareNames[hardwareID].Mode1.c_str()) == sTypeColor_White ||
										atoi(_hardwareNames[hardwareID].Mode1.c_str()) == sTypeColor_CW_WW))
								{
									DimmerType = "rel";
								}
							}
						}
						root["result"][ii]["DimmerType"] = DimmerType;
						root["result"][ii]["MaxDimLevel"] = maxDimLevel;
						root["result"][ii]["HaveGroupCmd"] = bHaveGroupCmd;
						root["result"][ii]["SwitchType"] = Switch_Type_Desc(switchtype);
						root["result"][ii]["SwitchTypeVal"] = switchtype;
						uint64_t camIDX = m_mainworker.m_cameras.IsDevSceneInCamera(0, sd[0]);
						root["result"][ii]["UsedByCamera"] = (camIDX != 0) ? true : false;
						if (camIDX != 0)
						{
							std::stringstream scidx;
							scidx << camIDX;
							root["result"][ii]["CameraIdx"] = scidx.str();
							root["result"][ii]["CameraAspect"] = m_mainworker.m_cameras.GetCameraAspectRatio(scidx.str());
						}

						bool bIsSubDevice = false;
						std::vector<std::vector<std::string>> resultSD;
						resultSD = m_sql.safe_query("SELECT ID FROM LightSubDevices WHERE (DeviceRowID=='%q')", sd[0].c_str());
						bIsSubDevice = (!resultSD.empty());

						root["result"][ii]["IsSubDevice"] = bIsSubDevice;

						std::string openStatus = "Open";
						std::string closedStatus = "Closed";
						if (switchtype == STYPE_Doorbell)
						{
							root["result"][ii]["TypeImg"] = "doorbell";
							root["result"][ii]["Status"] = ""; //"Pressed";
						}
						else if (switchtype == STYPE_DoorContact)
						{
							if (!CustomImage)
								root["result"][ii]["Image"] = "Door";
							root["result"][ii]["TypeImg"] = "door";
							bool bIsOn = IsLightSwitchOn(lstatus);
							root["result"][ii]["InternalState"] = (bIsOn == true) ? "Open" : "Closed";
							if (bIsOn)
							{
								lstatus = "Open";
							}
							else
							{
								lstatus = "Closed";
							}
							root["result"][ii]["Status"] = lstatus;
						}
						else if (switchtype == STYPE_DoorLock)
						{
							if (!CustomImage)
								root["result"][ii]["Image"] = "Door";
							root["result"][ii]["TypeImg"] = "door";
							bool bIsOn = IsLightSwitchOn(lstatus);
							root["result"][ii]["InternalState"] = (bIsOn == true) ? "Locked" : "Unlocked";
							if (bIsOn)
							{
								lstatus = "Locked";
							}
							else
							{
								lstatus = "Unlocked";
							}
							root["result"][ii]["Status"] = lstatus;
						}
						else if (switchtype == STYPE_DoorLockInverted)
						{
							if (!CustomImage)
								root["result"][ii]["Image"] = "Door";
							root["result"][ii]["TypeImg"] = "door";
							bool bIsOn = IsLightSwitchOn(lstatus);
							root["result"][ii]["InternalState"] = (bIsOn == true) ? "Unlocked" : "Locked";
							if (bIsOn)
							{
								lstatus = "Unlocked";
							}
							else
							{
								lstatus = "Locked";
							}
							root["result"][ii]["Status"] = lstatus;
						}
						else if (switchtype == STYPE_PushOn)
						{
							if (!CustomImage)
								root["result"][ii]["Image"] = "Push";
							root["result"][ii]["TypeImg"] = "push";
							root["result"][ii]["Status"] = "";
							root["result"][ii]["InternalState"] = (IsLightSwitchOn(lstatus) == true) ? "On" : "Off";
						}
						else if (switchtype == STYPE_PushOff)
						{
							if (!CustomImage)
								root["result"][ii]["Image"] = "Push";
							root["result"][ii]["TypeImg"] = "push";
							root["result"][ii]["Status"] = "";
							root["result"][ii]["TypeImg"] = "pushoff";
						}
						else if (switchtype == STYPE_X10Siren)
							root["result"][ii]["TypeImg"] = "siren";
						else if (switchtype == STYPE_SMOKEDETECTOR)
						{
							root["result"][ii]["TypeImg"] = "smoke";
							root["result"][ii]["SwitchTypeVal"] = STYPE_SMOKEDETECTOR;
							root["result"][ii]["SwitchType"] = Switch_Type_Desc(STYPE_SMOKEDETECTOR);
						}
						else if (switchtype == STYPE_Contact)
						{
							if (!CustomImage)
								root["result"][ii]["Image"] = "Contact";
							root["result"][ii]["TypeImg"] = "contact";
							bool bIsOn = IsLightSwitchOn(lstatus);
							if (bIsOn)
							{
								lstatus = "Open";
							}
							else
							{
								lstatus = "Closed";
							}
							root["result"][ii]["Status"] = lstatus;
						}
						else if (switchtype == STYPE_Media)
						{
							if ((pHardware != nullptr) && (pHardware->HwdType == HTYPE_LogitechMediaServer))
								root["result"][ii]["TypeImg"] = "LogitechMediaServer";
							else
								root["result"][ii]["TypeImg"] = "Media";
							root["result"][ii]["Status"] = Media_Player_States((_eMediaStatus)nValue);
							lstatus = sValue;
						}
						else if (
							(switchtype == STYPE_Blinds)
							|| (switchtype == STYPE_BlindsPercentage)
							|| (switchtype == STYPE_BlindsPercentageWithStop)
							|| (switchtype == STYPE_VenetianBlindsUS)
							|| (switchtype == STYPE_VenetianBlindsEU)
							)
						{
							root["result"][ii]["Image"] = "blinds";
							root["result"][ii]["TypeImg"] = "blinds";

							if (lstatus == "Close inline relay")
							{
								lstatus = "Close";
							}
							else if (lstatus == "Open inline relay")
							{
								lstatus = "Open";
							}
							else if (lstatus == "Stop inline relay")
							{
								lstatus = "Stop";
							}

							bool bReverseState = false;
							bool bReversePosition = false;

							auto itt = options.find("ReverseState");
							if (itt != options.end())
								bReverseState = (itt->second == "true");
							itt = options.find("ReversePosition");
							if (itt != options.end())
								bReversePosition = (itt->second == "true");

							if (bReversePosition)
							{
								LastLevel = 100 - LastLevel;
								if (lstatus.find("Set Level") == 0)
									lstatus = std_format("Set Level: %d %%", LastLevel);
							}

							if (bReverseState)
							{
								if (lstatus == "Open")
									lstatus = "Close";
								else if (lstatus == "Close")
									lstatus = "Open";
							}


							if (lstatus == "Close")
							{
								lstatus = closedStatus;
							}
							else if (lstatus == "Open")
							{
								lstatus = openStatus;
							}
							else if (lstatus == "Stop")
							{
								lstatus = "Stopped";
							}
							root["result"][ii]["Status"] = lstatus;

							root["result"][ii]["Level"] = LastLevel;
							int iLevel = round((float(maxDimLevel) / 100.0F) * LastLevel);
							root["result"][ii]["LevelInt"] = iLevel;

							root["result"][ii]["ReverseState"] = bReverseState;
							root["result"][ii]["ReversePosition"] = bReversePosition;
						}
						else if (switchtype == STYPE_Dimmer)
						{
							root["result"][ii]["TypeImg"] = "dimmer";
						}
						else if (switchtype == STYPE_Motion)
						{
							root["result"][ii]["TypeImg"] = "motion";
						}
						else if (switchtype == STYPE_Selector)
						{
							std::string selectorStyle = options["SelectorStyle"];
							std::string levelOffHidden = options["LevelOffHidden"];
							std::string levelNames = options["LevelNames"];
							std::string levelActions = options["LevelActions"];
							if (selectorStyle.empty())
							{
								selectorStyle.assign("0"); // default is 'button set'
							}
							if (levelOffHidden.empty())
							{
								levelOffHidden.assign("false"); // default is 'not hidden'
							}
							if (levelNames.empty())
							{
								levelNames.assign("Off"); // default is Off only
							}
							root["result"][ii]["TypeImg"] = "Light";
							root["result"][ii]["SelectorStyle"] = atoi(selectorStyle.c_str());
							root["result"][ii]["LevelOffHidden"] = (levelOffHidden == "true");
							root["result"][ii]["LevelNames"] = base64_encode(levelNames);
							root["result"][ii]["LevelActions"] = base64_encode(levelActions);
						}
						root["result"][ii]["Data"] = lstatus;
					}
					else if (dType == pTypeSecurity1)
					{
						std::string lstatus;
						int llevel = 0;
						bool bHaveDimmer = false;
						bool bHaveGroupCmd = false;
						int maxDimLevel = 0;

						GetLightStatus(dType, dSubType, switchtype, nValue, sValue, lstatus, llevel, bHaveDimmer, maxDimLevel, bHaveGroupCmd);

						root["result"][ii]["Status"] = lstatus;
						root["result"][ii]["HaveDimmer"] = bHaveDimmer;
						root["result"][ii]["MaxDimLevel"] = maxDimLevel;
						root["result"][ii]["HaveGroupCmd"] = bHaveGroupCmd;
						root["result"][ii]["SwitchType"] = "Security";
						root["result"][ii]["SwitchTypeVal"] = switchtype; // was 0?;
						root["result"][ii]["TypeImg"] = "security";
						root["result"][ii]["StrParam1"] = strParam1;
						root["result"][ii]["StrParam2"] = strParam2;
						root["result"][ii]["Protected"] = (iProtected != 0);

						if ((dSubType == sTypeKD101) || (dSubType == sTypeSA30) || (dSubType == sTypeRM174RF) || (switchtype == STYPE_SMOKEDETECTOR))
						{
							root["result"][ii]["SwitchTypeVal"] = STYPE_SMOKEDETECTOR;
							root["result"][ii]["TypeImg"] = "smoke";
							root["result"][ii]["SwitchType"] = Switch_Type_Desc(STYPE_SMOKEDETECTOR);
						}
						root["result"][ii]["Data"] = lstatus;
						root["result"][ii]["HaveTimeout"] = false;
					}
					else if (dType == pTypeSecurity2)
					{
						std::string lstatus;
						int llevel = 0;
						bool bHaveDimmer = false;
						bool bHaveGroupCmd = false;
						int maxDimLevel = 0;

						GetLightStatus(dType, dSubType, switchtype, nValue, sValue, lstatus, llevel, bHaveDimmer, maxDimLevel, bHaveGroupCmd);

						root["result"][ii]["Status"] = lstatus;
						root["result"][ii]["HaveDimmer"] = bHaveDimmer;
						root["result"][ii]["MaxDimLevel"] = maxDimLevel;
						root["result"][ii]["HaveGroupCmd"] = bHaveGroupCmd;
						root["result"][ii]["SwitchType"] = "Security";
						root["result"][ii]["SwitchTypeVal"] = switchtype; // was 0?;
						root["result"][ii]["TypeImg"] = "security";
						root["result"][ii]["StrParam1"] = strParam1;
						root["result"][ii]["StrParam2"] = strParam2;
						root["result"][ii]["Protected"] = (iProtected != 0);
						root["result"][ii]["Data"] = lstatus;
						root["result"][ii]["HaveTimeout"] = false;
					}
					else if (dType == pTypeEvohome || dType == pTypeEvohomeRelay)
					{
						std::string lstatus;
						int llevel = 0;
						bool bHaveDimmer = false;
						bool bHaveGroupCmd = false;
						int maxDimLevel = 0;

						GetLightStatus(dType, dSubType, switchtype, nValue, sValue, lstatus, llevel, bHaveDimmer, maxDimLevel, bHaveGroupCmd);

						root["result"][ii]["Status"] = lstatus;
						root["result"][ii]["HaveDimmer"] = bHaveDimmer;
						root["result"][ii]["MaxDimLevel"] = maxDimLevel;
						root["result"][ii]["HaveGroupCmd"] = bHaveGroupCmd;
						root["result"][ii]["SwitchType"] = "evohome";
						root["result"][ii]["SwitchTypeVal"] = switchtype; // was 0?;
						root["result"][ii]["TypeImg"] = "override_mini";
						root["result"][ii]["StrParam1"] = strParam1;
						root["result"][ii]["StrParam2"] = strParam2;
						root["result"][ii]["Protected"] = (iProtected != 0);

						root["result"][ii]["Data"] = lstatus;
						root["result"][ii]["HaveTimeout"] = false;

						if (dType == pTypeEvohomeRelay)
						{
							root["result"][ii]["SwitchType"] = "TPI";
							root["result"][ii]["Level"] = llevel;
							root["result"][ii]["LevelInt"] = atoi(sValue.c_str());
							if (root["result"][ii]["Unit"].asInt() > 100)
								root["result"][ii]["Protected"] = true;

							sprintf(szData, "%s: %d", lstatus.c_str(), atoi(sValue.c_str()));
							root["result"][ii]["Data"] = szData;
						}
					}
					else if ((dType == pTypeEvohomeZone) || (dType == pTypeEvohomeWater))
					{
						root["result"][ii]["HaveTimeout"] = bHaveTimeout;
						root["result"][ii]["TypeImg"] = "override_mini";

						std::vector<std::string> strarray;
						StringSplit(sValue, ";", strarray);
						if (strarray.size() >= 3)
						{
							int i = 0;
							double tempCelcius = atof(strarray[i++].c_str());
							double temp = ConvertTemperature(tempCelcius, tempsign);
							double tempSetPoint;
							root["result"][ii]["Temp"] = temp;
							if (dType == pTypeEvohomeWater && (strarray[i] == "Off" || strarray[i] == "On"))
							{
								root["result"][ii]["State"] = strarray[i++];
							}
							else
							{
								tempCelcius = atof(strarray[i++].c_str());
								tempSetPoint = ConvertTemperature(tempCelcius, tempsign);
								root["result"][ii]["SetPoint"] = tempSetPoint;
							}

							std::string strstatus = strarray[i++];
							root["result"][ii]["Status"] = strstatus;

							if ((dType == pTypeEvohomeZone || dType == pTypeEvohomeWater) && strarray.size() >= 4)
							{
								root["result"][ii]["Until"] = strarray[i++];
							}
							if (dType == pTypeEvohomeZone)
							{
								if (tempCelcius == 325.1)
									sprintf(szTmp, "Off");
								else
									sprintf(szTmp, "%.1f %c", tempSetPoint, tempsign);
								if (strarray.size() >= 4)
									sprintf(szData, "%.1f %c, (%s), %s until %s", temp, tempsign, szTmp, strstatus.c_str(), strarray[3].c_str());
								else
									sprintf(szData, "%.1f %c, (%s), %s", temp, tempsign, szTmp, strstatus.c_str());
							}
							else if (strarray.size() >= 4)
								sprintf(szData, "%.1f %c, %s, %s until %s", temp, tempsign, strarray[1].c_str(), strstatus.c_str(), strarray[3].c_str());
							else
								sprintf(szData, "%.1f %c, %s, %s", temp, tempsign, strarray[1].c_str(), strstatus.c_str());
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
						}
					}
					else if ((dType == pTypeTEMP) || (dType == pTypeRego6XXTemp))
					{
						double tvalue = ConvertTemperature(atof(sValue.c_str()), tempsign);
						root["result"][ii]["Temp"] = tvalue;
						sprintf(szData, "%.1f %c", tvalue, tempsign);
						root["result"][ii]["Data"] = szData;
						root["result"][ii]["HaveTimeout"] = bHaveTimeout;

						_tTrendCalculator::_eTendencyType tstate = _tTrendCalculator::_eTendencyType::TENDENCY_UNKNOWN;
						uint64_t tID = ((uint64_t)(hardwareID & 0x7FFFFFFF) << 32) | (devIdx & 0x7FFFFFFF);
						if (m_mainworker.m_trend_calculator.find(tID) != m_mainworker.m_trend_calculator.end())
						{
							tstate = m_mainworker.m_trend_calculator[tID].m_state;
						}
						root["result"][ii]["trend"] = (int)tstate;
					}
					else if (dType == pTypeThermostat1)
					{
						std::vector<std::string> strarray;
						StringSplit(sValue, ";", strarray);
						if (strarray.size() == 4)
						{
							double tvalue = ConvertTemperature(atof(strarray[0].c_str()), tempsign);
							root["result"][ii]["Temp"] = tvalue;
							sprintf(szData, "%.1f %c", tvalue, tempsign);
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
						}
					}
					else if ((dType == pTypeRFXSensor) && (dSubType == sTypeRFXSensorTemp))
					{
						double tvalue = ConvertTemperature(atof(sValue.c_str()), tempsign);
						root["result"][ii]["Temp"] = tvalue;
						sprintf(szData, "%.1f %c", tvalue, tempsign);
						root["result"][ii]["Data"] = szData;
						root["result"][ii]["TypeImg"] = "temperature";
						root["result"][ii]["HaveTimeout"] = bHaveTimeout;
						_tTrendCalculator::_eTendencyType tstate = _tTrendCalculator::_eTendencyType::TENDENCY_UNKNOWN;
						uint64_t tID = ((uint64_t)(hardwareID & 0x7FFFFFFF) << 32) | (devIdx & 0x7FFFFFFF);
						if (m_mainworker.m_trend_calculator.find(tID) != m_mainworker.m_trend_calculator.end())
						{
							tstate = m_mainworker.m_trend_calculator[tID].m_state;
						}
						root["result"][ii]["trend"] = (int)tstate;
					}
					else if (dType == pTypeHUM)
					{
						root["result"][ii]["Humidity"] = nValue;
						root["result"][ii]["HumidityStatus"] = RFX_Humidity_Status_Desc(atoi(sValue.c_str()));
						sprintf(szData, "Humidity %d %%", nValue);
						root["result"][ii]["Data"] = szData;
						root["result"][ii]["HaveTimeout"] = bHaveTimeout;
					}
					else if (dType == pTypeTEMP_HUM)
					{
						std::vector<std::string> strarray;
						StringSplit(sValue, ";", strarray);
						if (strarray.size() == 3)
						{
							double tempCelcius = atof(strarray[0].c_str());
							double temp = ConvertTemperature(tempCelcius, tempsign);
							int humidity = atoi(strarray[1].c_str());

							root["result"][ii]["Temp"] = temp;
							root["result"][ii]["Humidity"] = humidity;
							root["result"][ii]["HumidityStatus"] = RFX_Humidity_Status_Desc(atoi(strarray[2].c_str()));
							sprintf(szData, "%.1f %c, %d %%", temp, tempsign, atoi(strarray[1].c_str()));
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;

							// Calculate dew point

							sprintf(szTmp, "%.2f", ConvertTemperature(CalculateDewPoint(tempCelcius, humidity), tempsign));
							root["result"][ii]["DewPoint"] = szTmp;

							_tTrendCalculator::_eTendencyType tstate = _tTrendCalculator::_eTendencyType::TENDENCY_UNKNOWN;
							uint64_t tID = ((uint64_t)(hardwareID & 0x7FFFFFFF) << 32) | (devIdx & 0x7FFFFFFF);
							if (m_mainworker.m_trend_calculator.find(tID) != m_mainworker.m_trend_calculator.end())
							{
								tstate = m_mainworker.m_trend_calculator[tID].m_state;
							}
							root["result"][ii]["trend"] = (int)tstate;
						}
					}
					else if (dType == pTypeTEMP_HUM_BARO)
					{
						std::vector<std::string> strarray;
						StringSplit(sValue, ";", strarray);
						if (strarray.size() == 5)
						{
							double tempCelcius = atof(strarray[0].c_str());
							double temp = ConvertTemperature(tempCelcius, tempsign);
							int humidity = atoi(strarray[1].c_str());

							root["result"][ii]["Temp"] = temp;
							root["result"][ii]["Humidity"] = humidity;
							root["result"][ii]["HumidityStatus"] = RFX_Humidity_Status_Desc(atoi(strarray[2].c_str()));
							root["result"][ii]["Forecast"] = atoi(strarray[4].c_str());

							sprintf(szTmp, "%.2f", ConvertTemperature(CalculateDewPoint(tempCelcius, humidity), tempsign));
							root["result"][ii]["DewPoint"] = szTmp;

							if (dSubType == sTypeTHBFloat)
							{
								root["result"][ii]["Barometer"] = atof(strarray[3].c_str());
								root["result"][ii]["ForecastStr"] = RFX_WSForecast_Desc(atoi(strarray[4].c_str()));
							}
							else
							{
								root["result"][ii]["Barometer"] = atoi(strarray[3].c_str());
								root["result"][ii]["ForecastStr"] = RFX_Forecast_Desc(atoi(strarray[4].c_str()));
							}
							if (dSubType == sTypeTHBFloat)
							{
								sprintf(szData, "%.1f %c, %d %%, %.1f hPa", temp, tempsign, atoi(strarray[1].c_str()), atof(strarray[3].c_str()));
							}
							else
							{
								sprintf(szData, "%.1f %c, %d %%, %d hPa", temp, tempsign, atoi(strarray[1].c_str()), atoi(strarray[3].c_str()));
							}
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;

							_tTrendCalculator::_eTendencyType tstate = _tTrendCalculator::_eTendencyType::TENDENCY_UNKNOWN;
							uint64_t tID = ((uint64_t)(hardwareID & 0x7FFFFFFF) << 32) | (devIdx & 0x7FFFFFFF);
							if (m_mainworker.m_trend_calculator.find(tID) != m_mainworker.m_trend_calculator.end())
							{
								tstate = m_mainworker.m_trend_calculator[tID].m_state;
							}
							root["result"][ii]["trend"] = (int)tstate;
						}
					}
					else if (dType == pTypeTEMP_BARO)
					{
						std::vector<std::string> strarray;
						StringSplit(sValue, ";", strarray);
						if (strarray.size() >= 3)
						{
							double tvalue = ConvertTemperature(atof(strarray[0].c_str()), tempsign);
							root["result"][ii]["Temp"] = tvalue;
							int forecast = atoi(strarray[2].c_str());
							root["result"][ii]["Forecast"] = forecast;
							root["result"][ii]["ForecastStr"] = BMP_Forecast_Desc(forecast);
							root["result"][ii]["Barometer"] = atof(strarray[1].c_str());

							sprintf(szData, "%.1f %c, %.1f hPa", tvalue, tempsign, atof(strarray[1].c_str()));
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;

							_tTrendCalculator::_eTendencyType tstate = _tTrendCalculator::_eTendencyType::TENDENCY_UNKNOWN;
							uint64_t tID = ((uint64_t)(hardwareID & 0x7FFFFFFF) << 32) | (devIdx & 0x7FFFFFFF);
							if (m_mainworker.m_trend_calculator.find(tID) != m_mainworker.m_trend_calculator.end())
							{
								tstate = m_mainworker.m_trend_calculator[tID].m_state;
							}
							root["result"][ii]["trend"] = (int)tstate;
						}
					}
					else if (dType == pTypeUV)
					{
						std::vector<std::string> strarray;
						StringSplit(sValue, ";", strarray);
						if (strarray.size() == 2)
						{
							float UVI = static_cast<float>(atof(strarray[0].c_str()));
							root["result"][ii]["UVI"] = strarray[0];
							if (dSubType == sTypeUV3)
							{
								double tvalue = ConvertTemperature(atof(strarray[1].c_str()), tempsign);

								root["result"][ii]["Temp"] = tvalue;
								sprintf(szData, "%.1f UVI, %.1f&deg; %c", UVI, tvalue, tempsign);

								_tTrendCalculator::_eTendencyType tstate = _tTrendCalculator::_eTendencyType::TENDENCY_UNKNOWN;
								uint64_t tID = ((uint64_t)(hardwareID & 0x7FFFFFFF) << 32) | (devIdx & 0x7FFFFFFF);
								if (m_mainworker.m_trend_calculator.find(tID) != m_mainworker.m_trend_calculator.end())
								{
									tstate = m_mainworker.m_trend_calculator[tID].m_state;
								}
								root["result"][ii]["trend"] = (int)tstate;
							}
							else
							{
								sprintf(szData, "%.1f UVI", UVI);
							}
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
						}
					}
					else if (dType == pTypeWIND)
					{
						std::vector<std::string> strarray;
						StringSplit(sValue, ";", strarray);
						if (strarray.size() == 6)
						{
							root["result"][ii]["Direction"] = atof(strarray[0].c_str());
							root["result"][ii]["DirectionStr"] = strarray[1];

							if (dSubType != sTypeWIND5)
							{
								int intSpeed = atoi(strarray[2].c_str());
								if (m_sql.m_windunit != WINDUNIT_Beaufort)
								{
									sprintf(szTmp, "%.1f", float(intSpeed) * m_sql.m_windscale);
								}
								else
								{
									float windms = float(intSpeed) * 0.1F;
									sprintf(szTmp, "%d", MStoBeaufort(windms));
								}
								root["result"][ii]["Speed"] = szTmp;
							}

							// if (dSubType!=sTypeWIND6) //problem in RFXCOM firmware? gust=speed?
							{
								int intGust = atoi(strarray[3].c_str());
								if (m_sql.m_windunit != WINDUNIT_Beaufort)
								{
									sprintf(szTmp, "%.1f", float(intGust) * m_sql.m_windscale);
								}
								else
								{
									float gustms = float(intGust) * 0.1F;
									sprintf(szTmp, "%d", MStoBeaufort(gustms));
								}
								root["result"][ii]["Gust"] = szTmp;
							}
							if ((dSubType == sTypeWIND4) || (dSubType == sTypeWINDNoTemp))
							{
								if (dSubType == sTypeWIND4)
								{
									double tvalue = ConvertTemperature(atof(strarray[4].c_str()), tempsign);
									root["result"][ii]["Temp"] = tvalue;
								}
								double tvalue = ConvertTemperature(atof(strarray[5].c_str()), tempsign);
								root["result"][ii]["Chill"] = tvalue;

								_tTrendCalculator::_eTendencyType tstate = _tTrendCalculator::_eTendencyType::TENDENCY_UNKNOWN;
								uint64_t tID = ((uint64_t)(hardwareID & 0x7FFFFFFF) << 32) | (devIdx & 0x7FFFFFFF);
								if (m_mainworker.m_trend_calculator.find(tID) != m_mainworker.m_trend_calculator.end())
								{
									tstate = m_mainworker.m_trend_calculator[tID].m_state;
								}
								root["result"][ii]["trend"] = (int)tstate;
							}
							root["result"][ii]["Data"] = sValue;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
						}
					}
					else if (dType == pTypeRAIN)
					{
						std::vector<std::string> strarray;
						StringSplit(sValue, ";", strarray);
						if (strarray.size() == 2)
						{
							// get lowest value of today, and max rate
							time_t now = mytime(nullptr);
							struct tm ltime;
							localtime_r(&now, &ltime);
							char szDate[40];
							sprintf(szDate, "%04d-%02d-%02d", ltime.tm_year + 1900, ltime.tm_mon + 1, ltime.tm_mday);

							std::vector<std::vector<std::string>> result2;

							if (dSubType == sTypeRAINWU || dSubType == sTypeRAINByRate)
							{
								result2 = m_sql.safe_query("SELECT Total, Rate FROM Rain WHERE (DeviceRowID='%q' AND Date>='%q') ORDER BY ROWID DESC LIMIT 1",
									sd[0].c_str(), szDate);
							}
							else
							{
								result2 = m_sql.safe_query("SELECT MIN(Total), MAX(Total) FROM Rain WHERE (DeviceRowID='%q' AND Date>='%q')", sd[0].c_str(), szDate);
							}

							if (!result2.empty())
							{
								double total_real = 0;
								float rate = 0;
								std::vector<std::string> sd2 = result2[0];

								if (dSubType == sTypeRAINWU || dSubType == sTypeRAINByRate)
								{
									total_real = atof(sd2[0].c_str());
								}
								else
								{
									double total_min = atof(sd2[0].c_str());
									double total_max = atof(strarray[1].c_str());
									total_real = total_max - total_min;
								}

								total_real *= AddjMulti;
								if (dSubType == sTypeRAINByRate)
								{
									rate = static_cast<float>(atof(sd2[1].c_str()) / 10000.0F);
								}
								else
								{
									rate = (static_cast<float>(atof(strarray[0].c_str())) / 100.0F) * float(AddjMulti);
								}

								sprintf(szTmp, "%.1f", total_real);
								root["result"][ii]["Rain"] = szTmp;
								sprintf(szTmp, "%g", rate);
								root["result"][ii]["RainRate"] = szTmp;
								root["result"][ii]["Data"] = sValue;
								root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							}
							else
							{
								root["result"][ii]["Rain"] = "0";
								root["result"][ii]["RainRate"] = "0";
								root["result"][ii]["Data"] = "0";
								root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							}
						}
					}
					else if (dType == pTypeRFXMeter)
					{
						std::string ValueQuantity = options["ValueQuantity"];
						std::string ValueUnits = options["ValueUnits"];
						float divider = m_sql.GetCounterDivider(int(metertype), int(dType), float(AddjValue2));

						if (ValueQuantity.empty())
						{
							ValueQuantity = "Custom";
						}

						// get value of today
						time_t now = mytime(nullptr);
						struct tm ltime;
						localtime_r(&now, &ltime);
						char szDate[40];
						sprintf(szDate, "%04d-%02d-%02d", ltime.tm_year + 1900, ltime.tm_mon + 1, ltime.tm_mday);

						std::vector<std::vector<std::string>> result2;
						strcpy(szTmp, "0");
						result2 = m_sql.safe_query("SELECT Value FROM Meter WHERE (DeviceRowID='%q' AND Date>='%q') ORDER BY Date LIMIT 1", sd[0].c_str(), szDate);
						if (!result2.empty())
						{
							std::vector<std::string> sd2 = result2[0];

							int64_t total_first = std::stoll(sd2[0]);
							int64_t total_last = std::stoll(sValue);
							int64_t total_real = total_last - total_first;

							sprintf(szTmp, "%" PRId64, total_real);

							double musage = 0.0F;
							switch (metertype)
							{
							case MTYPE_ENERGY:
							case MTYPE_ENERGY_GENERATED:
								musage = double(total_real) / divider;
								sprintf(szTmp, "%.3f kWh", musage);
								break;
							case MTYPE_GAS:
								musage = double(total_real) / divider;
								sprintf(szTmp, "%.3f m3", musage);
								break;
							case MTYPE_WATER:
								musage = double(total_real) / (divider / 1000.0F);
								sprintf(szTmp, "%d Liter", round(musage));
								break;
							case MTYPE_COUNTER:
								musage = double(total_real) / divider;
								sprintf(szTmp, "%.10g", musage);
								if (!ValueUnits.empty())
								{
									strcat(szTmp, " ");
									strcat(szTmp, ValueUnits.c_str());
								}
								break;
							default:
								strcpy(szTmp, "?");
								break;
							}
						}
						root["result"][ii]["CounterToday"] = szTmp;

						root["result"][ii]["SwitchTypeVal"] = metertype;
						root["result"][ii]["HaveTimeout"] = bHaveTimeout;
						root["result"][ii]["ValueQuantity"] = ValueQuantity;
						root["result"][ii]["ValueUnits"] = ValueUnits;
						root["result"][ii]["Divider"] = divider;

						double meteroffset = AddjValue;

						double dvalue = static_cast<double>(atof(sValue.c_str()));

						switch (metertype)
						{
						case MTYPE_ENERGY:
						case MTYPE_ENERGY_GENERATED:
							sprintf(szTmp, "%.3f kWh", meteroffset + (dvalue / divider));
							root["result"][ii]["Data"] = szTmp;
							root["result"][ii]["Counter"] = szTmp;
							break;
						case MTYPE_GAS:
							sprintf(szTmp, "%.3f m3", meteroffset + (dvalue / divider));
							root["result"][ii]["Data"] = szTmp;
							root["result"][ii]["Counter"] = szTmp;
							break;
						case MTYPE_WATER:
							sprintf(szTmp, "%.3f m3", meteroffset + (dvalue / divider));
							root["result"][ii]["Data"] = szTmp;
							root["result"][ii]["Counter"] = szTmp;
							break;
						case MTYPE_COUNTER:
							sprintf(szTmp, "%.10g", meteroffset + (dvalue / divider));
							if (!ValueUnits.empty())
							{
								strcat(szTmp, " ");
								strcat(szTmp, ValueUnits.c_str());
							}
							root["result"][ii]["Data"] = szTmp;
							root["result"][ii]["Counter"] = szTmp;
							break;
						default:
							root["result"][ii]["Data"] = "?";
							root["result"][ii]["Counter"] = "?";
							break;
						}
					}
					else if (dType == pTypeYouLess)
					{
						std::string ValueQuantity = options["ValueQuantity"];
						std::string ValueUnits = options["ValueUnits"];
						if (ValueQuantity.empty())
						{
							ValueQuantity = "Custom";
						}

						double musage = 0;
						double divider = m_sql.GetCounterDivider(int(metertype), int(dType), float(AddjValue2));

						// get value of today
						time_t now = mytime(nullptr);
						struct tm ltime;
						localtime_r(&now, &ltime);
						char szDate[40];
						sprintf(szDate, "%04d-%02d-%02d", ltime.tm_year + 1900, ltime.tm_mon + 1, ltime.tm_mday);

						std::vector<std::vector<std::string>> result2;
						strcpy(szTmp, "0");
						result2 = m_sql.safe_query("SELECT MIN(Value), MAX(Value) FROM Meter WHERE (DeviceRowID='%q' AND Date>='%q')", sd[0].c_str(), szDate);
						if (!result2.empty())
						{
							std::vector<std::string> sd2 = result2[0];

							uint64_t total_min = std::stoull(sd2[0]);
							uint64_t total_max = std::stoull(sd2[1]);
							uint64_t total_real = total_max - total_min;

							sprintf(szTmp, "%" PRIu64, total_real);

							musage = 0;
							switch (metertype)
							{
							case MTYPE_ENERGY:
							case MTYPE_ENERGY_GENERATED:
								musage = double(total_real) / divider;
								sprintf(szTmp, "%.3f kWh", musage);
								break;
							case MTYPE_GAS:
								musage = double(total_real) / divider;
								sprintf(szTmp, "%.3f m3", musage);
								break;
							case MTYPE_WATER:
								musage = double(total_real) / divider;
								sprintf(szTmp, "%.3f m3", musage);
								break;
							case MTYPE_COUNTER:
								sprintf(szTmp, "%.10g", double(total_real) / divider);
								if (!ValueUnits.empty())
								{
									strcat(szTmp, " ");
									strcat(szTmp, ValueUnits.c_str());
								}
								break;
							default:
								strcpy(szTmp, "0");
								break;
							}
						}
						root["result"][ii]["CounterToday"] = szTmp;

						std::vector<std::string> splitresults;
						StringSplit(sValue, ";", splitresults);
						if (splitresults.size() < 2)
							continue;

						uint64_t total_actual = std::stoull(splitresults[0]);
						musage = 0;
						switch (metertype)
						{
						case MTYPE_ENERGY:
						case MTYPE_ENERGY_GENERATED:
							musage = double(total_actual) / divider;
							sprintf(szTmp, "%.03f", musage);
							break;
						case MTYPE_GAS:
						case MTYPE_WATER:
							musage = double(total_actual) / divider;
							sprintf(szTmp, "%.03f", musage);
							break;
						case MTYPE_COUNTER:
							sprintf(szTmp, "%.10g", double(total_actual) / divider);
							break;
						default:
							strcpy(szTmp, "0");
							break;
						}
						root["result"][ii]["Counter"] = szTmp;

						root["result"][ii]["SwitchTypeVal"] = metertype;

						uint64_t acounter = std::stoull(sValue);
						musage = 0;
						switch (metertype)
						{
						case MTYPE_ENERGY:
						case MTYPE_ENERGY_GENERATED:
							musage = double(acounter) / divider;
							sprintf(szTmp, "%.3f kWh %s Watt", musage, splitresults[1].c_str());
							break;
						case MTYPE_GAS:
							musage = double(acounter) / divider;
							sprintf(szTmp, "%.3f m3", musage);
							break;
						case MTYPE_WATER:
							musage = double(acounter) / divider;
							sprintf(szTmp, "%.3f m3", musage);
							break;
						case MTYPE_COUNTER:
							sprintf(szTmp, "%.10g", double(acounter) / divider);
							if (!ValueUnits.empty())
							{
								strcat(szTmp, " ");
								strcat(szTmp, ValueUnits.c_str());
							}
							break;
						default:
							strcpy(szTmp, "0");
							break;
						}
						root["result"][ii]["Data"] = szTmp;
						root["result"][ii]["ValueQuantity"] = ValueQuantity;
						root["result"][ii]["ValueUnits"] = ValueUnits;
						root["result"][ii]["Divider"] = divider;

						switch (metertype)
						{
						case MTYPE_ENERGY:
						case MTYPE_ENERGY_GENERATED:
							sprintf(szTmp, "%s Watt", splitresults[1].c_str());
							break;
						case MTYPE_GAS:
							sprintf(szTmp, "%s m3", splitresults[1].c_str());
							break;
						case MTYPE_WATER:
							sprintf(szTmp, "%s m3", splitresults[1].c_str());
							break;
						case MTYPE_COUNTER:
							sprintf(szTmp, "%s", splitresults[1].c_str());
							break;
						default:
							strcpy(szTmp, "0");
							break;
						}

						root["result"][ii]["Usage"] = szTmp;
						root["result"][ii]["HaveTimeout"] = bHaveTimeout;
					}
					else if (dType == pTypeP1Power)
					{
						std::vector<std::string> splitresults;
						StringSplit(sValue, ";", splitresults);
						if (splitresults.size() != 6)
						{
							root["result"][ii]["SwitchTypeVal"] = MTYPE_ENERGY;
							root["result"][ii]["Counter"] = "0";
							root["result"][ii]["CounterDeliv"] = "0";
							root["result"][ii]["Usage"] = "Invalid";
							root["result"][ii]["UsageDeliv"] = "Invalid";
							root["result"][ii]["Data"] = "Invalid!: " + sValue;
							root["result"][ii]["HaveTimeout"] = true;
							root["result"][ii]["CounterToday"] = "Invalid";
							root["result"][ii]["CounterDelivToday"] = "Invalid";
						}
						else
						{
							float EnergyDivider = 1000.0F;
							int tValue;
							if (m_sql.GetPreferencesVar("MeterDividerEnergy", tValue))
							{
								EnergyDivider = float(tValue);
							}

							uint64_t powerusage1 = std::stoull(splitresults[0]);
							uint64_t powerusage2 = std::stoull(splitresults[1]);
							uint64_t powerdeliv1 = std::stoull(splitresults[2]);
							uint64_t powerdeliv2 = std::stoull(splitresults[3]);
							uint64_t usagecurrent = std::stoull(splitresults[4]);
							uint64_t delivcurrent = std::stoull(splitresults[5]);

							powerdeliv1 = (powerdeliv1 < 10) ? 0 : powerdeliv1;
							powerdeliv2 = (powerdeliv2 < 10) ? 0 : powerdeliv2;

							uint64_t powerusage = powerusage1 + powerusage2;
							uint64_t powerdeliv = powerdeliv1 + powerdeliv2;
							if (powerdeliv < 2)
								powerdeliv = 0;

							double musage = 0;

							root["result"][ii]["SwitchTypeVal"] = MTYPE_ENERGY;
							musage = double(powerusage) / EnergyDivider;
							sprintf(szTmp, "%.03f", musage);
							root["result"][ii]["Counter"] = szTmp;
							musage = double(powerdeliv) / EnergyDivider;
							sprintf(szTmp, "%.03f", musage);
							root["result"][ii]["CounterDeliv"] = szTmp;

							if (bHaveTimeout)
							{
								usagecurrent = 0;
								delivcurrent = 0;
							}
							sprintf(szTmp, "%" PRIu64 " Watt", usagecurrent);
							root["result"][ii]["Usage"] = szTmp;
							sprintf(szTmp, "%" PRIu64 " Watt", delivcurrent);
							root["result"][ii]["UsageDeliv"] = szTmp;
							root["result"][ii]["Data"] = sValue;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;

							// get value of today
							time_t now = mytime(nullptr);
							struct tm ltime;
							localtime_r(&now, &ltime);
							char szDate[40];
							sprintf(szDate, "%04d-%02d-%02d", ltime.tm_year + 1900, ltime.tm_mon + 1, ltime.tm_mday);

							std::vector<std::vector<std::string>> result2;
							strcpy(szTmp, "0");
							result2 = m_sql.safe_query("SELECT MIN(Value1), MIN(Value2), MIN(Value5), MIN(Value6) FROM MultiMeter WHERE (DeviceRowID='%q' AND Date>='%q')",
								sd[0].c_str(), szDate);
							if (!result2.empty())
							{
								std::vector<std::string> sd2 = result2[0];

								uint64_t total_min_usage_1 = std::stoull(sd2[0]);
								uint64_t total_min_deliv_1 = std::stoull(sd2[1]);
								uint64_t total_min_usage_2 = std::stoull(sd2[2]);
								uint64_t total_min_deliv_2 = std::stoull(sd2[3]);
								uint64_t total_real_usage, total_real_deliv;

								total_min_deliv_1 = (total_min_deliv_1 < 10) ? 0 : total_min_deliv_1;
								total_min_deliv_2 = (total_min_deliv_2 < 10) ? 0 : total_min_deliv_2;

								total_real_usage = powerusage - (total_min_usage_1 + total_min_usage_2);
								total_real_deliv = powerdeliv - (total_min_deliv_1 + total_min_deliv_2);

								if (total_real_deliv < 2)
									total_real_deliv = 0;

								musage = double(total_real_usage) / EnergyDivider;
								sprintf(szTmp, "%.3f kWh", musage);
								root["result"][ii]["CounterToday"] = szTmp;
								musage = double(total_real_deliv) / EnergyDivider;
								sprintf(szTmp, "%.3f kWh", musage);
								root["result"][ii]["CounterDelivToday"] = szTmp;
							}
							else
							{
								sprintf(szTmp, "%.3f kWh", 0.0F);
								root["result"][ii]["CounterToday"] = szTmp;
								root["result"][ii]["CounterDelivToday"] = szTmp;
							}
						}
					}
					else if (dType == pTypeP1Gas)
					{
						root["result"][ii]["SwitchTypeVal"] = MTYPE_GAS;

						// get lowest value of today
						time_t now = mytime(nullptr);
						struct tm ltime;
						localtime_r(&now, &ltime);
						char szDate[40];
						sprintf(szDate, "%04d-%02d-%02d", ltime.tm_year + 1900, ltime.tm_mon + 1, ltime.tm_mday);

						std::vector<std::vector<std::string>> result2;

						float divider = m_sql.GetCounterDivider(int(metertype), int(dType), float(AddjValue2));

						strcpy(szTmp, "0");
						result2 = m_sql.safe_query("SELECT MIN(Value) FROM Meter WHERE (DeviceRowID='%q' AND Date>='%q')", sd[0].c_str(), szDate);
						if (!result2.empty())
						{
							std::vector<std::string> sd2 = result2[0];

							uint64_t total_min_gas = std::stoull(sd2[0]);
							uint64_t gasactual;
							try
							{
								gasactual = std::stoull(sValue);
							}
							catch (std::invalid_argument e)
							{
								_log.Log(LOG_ERROR, "Gas - invalid value: '%s'", sValue.c_str());
								continue;
							}
							uint64_t total_real_gas = gasactual - total_min_gas;

							double musage = double(gasactual) / divider;
							sprintf(szTmp, "%.03f", musage);
							root["result"][ii]["Counter"] = szTmp;
							musage = double(total_real_gas) / divider;
							sprintf(szTmp, "%.03f m3", musage);
							root["result"][ii]["CounterToday"] = szTmp;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							sprintf(szTmp, "%.03f", atof(sValue.c_str()) / divider);
							root["result"][ii]["Data"] = szTmp;
						}
						else
						{
							sprintf(szTmp, "%.03f", 0.0F);
							root["result"][ii]["Counter"] = szTmp;
							sprintf(szTmp, "%.03f m3", 0.0F);
							root["result"][ii]["CounterToday"] = szTmp;
							sprintf(szTmp, "%.03f", atof(sValue.c_str()) / divider);
							root["result"][ii]["Data"] = szTmp;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
						}
					}
					else if (dType == pTypeCURRENT)
					{
						std::vector<std::string> strarray;
						StringSplit(sValue, ";", strarray);
						if (strarray.size() == 3)
						{
							// CM113
							int displaytype = 0;
							int voltage = 230;
							m_sql.GetPreferencesVar("CM113DisplayType", displaytype);
							m_sql.GetPreferencesVar("ElectricVoltage", voltage);

							double val1 = atof(strarray[0].c_str());
							double val2 = atof(strarray[1].c_str());
							double val3 = atof(strarray[2].c_str());

							if (displaytype == 0)
							{
								if ((val2 == 0) && (val3 == 0))
									sprintf(szData, "%.1f A", val1);
								else
									sprintf(szData, "%.1f A, %.1f A, %.1f A", val1, val2, val3);
							}
							else
							{
								if ((val2 == 0) && (val3 == 0))
									sprintf(szData, "%d Watt", int(val1 * voltage));
								else
									sprintf(szData, "%d Watt, %d Watt, %d Watt", int(val1 * voltage), int(val2 * voltage), int(val3 * voltage));
							}
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["displaytype"] = displaytype;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
						}
					}
					else if (dType == pTypeCURRENTENERGY)
					{
						std::vector<std::string> strarray;
						StringSplit(sValue, ";", strarray);
						if (strarray.size() == 4)
						{
							// CM180i
							int displaytype = 0;
							int voltage = 230;
							m_sql.GetPreferencesVar("CM113DisplayType", displaytype);
							m_sql.GetPreferencesVar("ElectricVoltage", voltage);

							double total = atof(strarray[3].c_str());
							if (displaytype == 0)
							{
								sprintf(szData, "%.1f A, %.1f A, %.1f A", atof(strarray[0].c_str()), atof(strarray[1].c_str()), atof(strarray[2].c_str()));
							}
							else
							{
								sprintf(szData, "%d Watt, %d Watt, %d Watt", int(atof(strarray[0].c_str()) * voltage), int(atof(strarray[1].c_str()) * voltage),
									int(atof(strarray[2].c_str()) * voltage));
							}
							if (total > 0)
							{
								sprintf(szTmp, ", Total: %.3f kWh", total / 1000.0F);
								strcat(szData, szTmp);
							}
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["displaytype"] = displaytype;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
						}
					}
					else if (((dType == pTypeENERGY) || (dType == pTypePOWER)) || ((dType == pTypeGeneral) && (dSubType == sTypeKwh)))
					{
						std::vector<std::string> strarray;
						StringSplit(sValue, ";", strarray);
						if (strarray.size() == 2)
						{
							double total = atof(strarray[1].c_str()) / 1000;

							time_t now = mytime(nullptr);
							struct tm ltime;
							localtime_r(&now, &ltime);
							char szDate[40];
							sprintf(szDate, "%04d-%02d-%02d", ltime.tm_year + 1900, ltime.tm_mon + 1, ltime.tm_mday);

							std::vector<std::vector<std::string>> result2;
							strcpy(szTmp, "0");
							// get the first value of the day instead of the minimum value, because counter can also decrease
							// result2 = m_sql.safe_query("SELECT MIN(Value) FROM Meter WHERE (DeviceRowID='%q' AND Date>='%q')",
							result2 = m_sql.safe_query("SELECT Value FROM Meter WHERE (DeviceRowID='%q' AND Date>='%q') ORDER BY Date LIMIT 1", sd[0].c_str(), szDate);
							if (!result2.empty())
							{
								float divider = m_sql.GetCounterDivider(int(metertype), int(dType), float(AddjValue2));

								std::vector<std::string> sd2 = result2[0];
								double minimum = atof(sd2[0].c_str()) / divider;

								sprintf(szData, "%.3f kWh", total);
								root["result"][ii]["Data"] = szData;
								if ((dType == pTypeENERGY) || (dType == pTypePOWER))
								{
									sprintf(szData, "%ld Watt", atol(strarray[0].c_str()));
								}
								else
								{
									sprintf(szData, "%g Watt", atof(strarray[0].c_str()));
								}
								root["result"][ii]["Usage"] = szData;
								root["result"][ii]["HaveTimeout"] = bHaveTimeout;
								sprintf(szTmp, "%.3f kWh", total - minimum);
								root["result"][ii]["CounterToday"] = szTmp;
							}
							else
							{
								sprintf(szData, "%.3f kWh", total);
								root["result"][ii]["Data"] = szData;
								if ((dType == pTypeENERGY) || (dType == pTypePOWER))
								{
									sprintf(szData, "%ld Watt", atol(strarray[0].c_str()));
								}
								else
								{
									sprintf(szData, "%g Watt", atof(strarray[0].c_str()));
								}
								root["result"][ii]["Usage"] = szData;
								root["result"][ii]["HaveTimeout"] = bHaveTimeout;
								sprintf(szTmp, "%d kWh", 0);
								root["result"][ii]["CounterToday"] = szTmp;
							}
						}
						root["result"][ii]["TypeImg"] = "current";
						root["result"][ii]["SwitchTypeVal"] = switchtype;		    // MTYPE_ENERGY
						root["result"][ii]["EnergyMeterMode"] = options["EnergyMeterMode"]; // for alternate Energy Reading
					}
					else if (dType == pTypeAirQuality)
					{
						if (bHaveTimeout)
							nValue = 0;
						sprintf(szTmp, "%d ppm", nValue);
						root["result"][ii]["Data"] = szTmp;
						root["result"][ii]["HaveTimeout"] = bHaveTimeout;
						int airquality = nValue;
						if (airquality < 700)
							root["result"][ii]["Quality"] = "Excellent";
						else if (airquality < 900)
							root["result"][ii]["Quality"] = "Good";
						else if (airquality < 1100)
							root["result"][ii]["Quality"] = "Fair";
						else if (airquality < 1600)
							root["result"][ii]["Quality"] = "Mediocre";
						else
							root["result"][ii]["Quality"] = "Bad";
					}
					else if (dType == pTypeThermostat)
					{
						if (dSubType == sTypeThermSetpoint)
						{
							bHasTimers = m_sql.HasTimers(sd[0]);

							double tempCelcius = atof(sValue.c_str());
							double temp = ConvertTemperature(tempCelcius, tempsign);

							sprintf(szTmp, "%.1f", temp);
							root["result"][ii]["Data"] = szTmp;
							root["result"][ii]["SetPoint"] = szTmp;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							root["result"][ii]["TypeImg"] = "override_mini";
						}
					}
					else if (dType == pTypeRadiator1)
					{
						if (dSubType == sTypeSmartwares)
						{
							bHasTimers = m_sql.HasTimers(sd[0]);

							double tempCelcius = atof(sValue.c_str());
							double temp = ConvertTemperature(tempCelcius, tempsign);

							sprintf(szTmp, "%.1f", temp);
							root["result"][ii]["Data"] = szTmp;
							root["result"][ii]["SetPoint"] = szTmp;
							root["result"][ii]["HaveTimeout"] = false; // this device does not provide feedback, so no timeout!
							root["result"][ii]["TypeImg"] = "override_mini";
						}
					}
					else if (dType == pTypeGeneral)
					{
						if (dSubType == sTypeVisibility)
						{
							float vis = static_cast<float>(atof(sValue.c_str()));
							if (metertype == 0)
							{
								// km
								sprintf(szTmp, "%.1f km", vis);
							}
							else
							{
								// miles
								sprintf(szTmp, "%.1f mi", vis * 0.6214F);
							}
							root["result"][ii]["Data"] = szTmp;
							root["result"][ii]["Visibility"] = atof(sValue.c_str());
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							root["result"][ii]["TypeImg"] = "visibility";
							root["result"][ii]["SwitchTypeVal"] = metertype;
						}
						else if (dSubType == sTypeDistance)
						{
							float vis = static_cast<float>(atof(sValue.c_str()));
							if (metertype == 0)
							{
								// Metric
								sprintf(szTmp, "%.1f cm", vis);
							}
							else
							{
								// Imperial
								sprintf(szTmp, "%.1f in", vis * 0.3937007874015748F);
							}
							root["result"][ii]["Data"] = szTmp;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							root["result"][ii]["TypeImg"] = "visibility";
							root["result"][ii]["SwitchTypeVal"] = metertype;
						}
						else if (dSubType == sTypeSolarRadiation)
						{
							float radiation = static_cast<float>(atof(sValue.c_str()));
							sprintf(szTmp, "%.1f Watt/m2", radiation);
							root["result"][ii]["Data"] = szTmp;
							root["result"][ii]["Radiation"] = atof(sValue.c_str());
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							root["result"][ii]["TypeImg"] = "radiation";
							root["result"][ii]["SwitchTypeVal"] = metertype;
						}
						else if (dSubType == sTypeSoilMoisture)
						{
							sprintf(szTmp, "%d cb", nValue);
							root["result"][ii]["Data"] = szTmp;
							root["result"][ii]["Desc"] = Get_Moisture_Desc(nValue);
							root["result"][ii]["TypeImg"] = "moisture";
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							root["result"][ii]["SwitchTypeVal"] = metertype;
						}
						else if (dSubType == sTypeLeafWetness)
						{
							sprintf(szTmp, "%d", nValue);
							root["result"][ii]["Data"] = szTmp;
							root["result"][ii]["TypeImg"] = "leaf";
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							root["result"][ii]["SwitchTypeVal"] = metertype;
						}
						else if (dSubType == sTypeSystemTemp)
						{
							double tvalue = ConvertTemperature(atof(sValue.c_str()), tempsign);
							root["result"][ii]["Temp"] = tvalue;
							sprintf(szData, "%.1f %c", tvalue, tempsign);
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							if (!CustomImage)
								root["result"][ii]["Image"] = "Computer";
							root["result"][ii]["TypeImg"] = "temperature";
							root["result"][ii]["Type"] = "temperature";
							_tTrendCalculator::_eTendencyType tstate = _tTrendCalculator::_eTendencyType::TENDENCY_UNKNOWN;
							uint64_t tID = ((uint64_t)(hardwareID & 0x7FFFFFFF) << 32) | (devIdx & 0x7FFFFFFF);
							if (m_mainworker.m_trend_calculator.find(tID) != m_mainworker.m_trend_calculator.end())
							{
								tstate = m_mainworker.m_trend_calculator[tID].m_state;
							}
							root["result"][ii]["trend"] = (int)tstate;
						}
						else if (dSubType == sTypePercentage)
						{
							sprintf(szData, "%g%%", atof(sValue.c_str()));
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							root["result"][ii]["TypeImg"] = "hardware";
						}
						else if (dSubType == sTypeWaterflow)
						{
							sprintf(szData, "%g l/min", atof(sValue.c_str()));
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							if (!CustomImage)
								root["result"][ii]["Image"] = "Moisture";
							root["result"][ii]["TypeImg"] = "moisture";
						}
						else if (dSubType == sTypeCustom)
						{
							std::string szAxesLabel;
							int SensorType = 1;
							std::vector<std::string> sResults;
							StringSplit(sOptions, ";", sResults);

							if (sResults.size() == 2)
							{
								SensorType = atoi(sResults[0].c_str());
								szAxesLabel = sResults[1];
							}
							sprintf(szData, "%g %s", atof(sValue.c_str()), szAxesLabel.c_str());
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["SensorType"] = SensorType;
							root["result"][ii]["SensorUnit"] = szAxesLabel;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;

							if (!CustomImage)
								root["result"][ii]["Image"] = "Custom";
							root["result"][ii]["TypeImg"] = "Custom";
						}
						else if (dSubType == sTypeFan)
						{
							sprintf(szData, "%d RPM", atoi(sValue.c_str()));
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							if (!CustomImage)
								root["result"][ii]["Image"] = "Fan";
							root["result"][ii]["TypeImg"] = "Fan";
						}
						else if (dSubType == sTypeSoundLevel)
						{
							sprintf(szData, "%d dB", atoi(sValue.c_str()));
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["TypeImg"] = "Speaker";
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
						}
						else if (dSubType == sTypeVoltage)
						{
							sprintf(szData, "%g V", atof(sValue.c_str()));
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["TypeImg"] = "current";
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							root["result"][ii]["Voltage"] = atof(sValue.c_str());
						}
						else if (dSubType == sTypeCurrent)
						{
							sprintf(szData, "%g A", atof(sValue.c_str()));
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["TypeImg"] = "current";
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							root["result"][ii]["Current"] = atof(sValue.c_str());
						}
						else if (dSubType == sTypeTextStatus)
						{
							root["result"][ii]["Data"] = sValue;
							root["result"][ii]["TypeImg"] = "text";
							root["result"][ii]["HaveTimeout"] = false;
							root["result"][ii]["ShowNotifications"] = false;
						}
						else if (dSubType == sTypeAlert)
						{
							if (nValue > 4)
								nValue = 4;
							sprintf(szData, "Level: %d", nValue);
							root["result"][ii]["Data"] = szData;
							if (!sValue.empty())
								root["result"][ii]["Data"] = sValue;
							else
								root["result"][ii]["Data"] = Get_Alert_Desc(nValue);
							root["result"][ii]["TypeImg"] = "Alert";
							root["result"][ii]["Level"] = nValue;
							root["result"][ii]["HaveTimeout"] = false;
						}
						else if (dSubType == sTypePressure)
						{
							sprintf(szData, "%.1f Bar", atof(sValue.c_str()));
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["TypeImg"] = "gauge";
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							root["result"][ii]["Pressure"] = atof(sValue.c_str());
						}
						else if (dSubType == sTypeBaro)
						{
							std::vector<std::string> tstrarray;
							StringSplit(sValue, ";", tstrarray);
							if (tstrarray.empty())
								continue;
							sprintf(szData, "%g hPa", atof(tstrarray[0].c_str()));
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["TypeImg"] = "gauge";
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							if (tstrarray.size() > 1)
							{
								root["result"][ii]["Barometer"] = atof(tstrarray[0].c_str());
								int forecast = atoi(tstrarray[1].c_str());
								root["result"][ii]["Forecast"] = forecast;
								root["result"][ii]["ForecastStr"] = BMP_Forecast_Desc(forecast);
							}
						}
						else if (dSubType == sTypeCounterIncremental)
						{
							std::string ValueQuantity = options["ValueQuantity"];
							std::string ValueUnits = options["ValueUnits"];
							if (ValueQuantity.empty())
							{
								ValueQuantity = "Custom";
							}

							double divider = m_sql.GetCounterDivider(int(metertype), int(dType), float(AddjValue2));

							// get value of today
							time_t now = mytime(nullptr);
							struct tm ltime;
							localtime_r(&now, &ltime);
							char szDate[40];
							sprintf(szDate, "%04d-%02d-%02d", ltime.tm_year + 1900, ltime.tm_mon + 1, ltime.tm_mday);

							std::vector<std::vector<std::string>> result2;
							strcpy(szTmp, "0");
							result2 = m_sql.safe_query("SELECT Value FROM Meter WHERE (DeviceRowID='%q' AND Date>='%q') ORDER BY Date LIMIT 1", sd[0].c_str(), szDate);
							if (!result2.empty())
							{
								std::vector<std::string> sd2 = result2[0];

								int64_t total_first = std::stoll(sd2[0]);
								int64_t total_last = std::stoll(sValue);
								int64_t total_real = total_last - total_first;

								double musage = 0;
								switch (metertype)
								{
								case MTYPE_ENERGY:
								case MTYPE_ENERGY_GENERATED:
									musage = double(total_real) / divider;
									sprintf(szTmp, "%.3f kWh", musage);
									break;
								case MTYPE_GAS:
									musage = double(total_real) / divider;
									sprintf(szTmp, "%.3f m3", musage);
									break;
								case MTYPE_WATER:
									musage = double(total_real) / divider;
									sprintf(szTmp, "%.3f m3", musage);
									break;
								case MTYPE_COUNTER:
									sprintf(szTmp, "%.10g", double(total_real) / divider);
									if (!ValueUnits.empty())
									{
										strcat(szTmp, " ");
										strcat(szTmp, ValueUnits.c_str());
									}
									break;
								default:
									strcpy(szTmp, "0");
									break;
								}
							}
							root["result"][ii]["Counter"] = sValue;
							root["result"][ii]["CounterToday"] = szTmp;
							root["result"][ii]["SwitchTypeVal"] = metertype;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							root["result"][ii]["TypeImg"] = "counter";
							root["result"][ii]["ValueQuantity"] = ValueQuantity;
							root["result"][ii]["ValueUnits"] = ValueUnits;
							root["result"][ii]["Divider"] = divider;

							double dvalue = static_cast<double>(atof(sValue.c_str()));
							double meteroffset = AddjValue;

							switch (metertype)
							{
							case MTYPE_ENERGY:
							case MTYPE_ENERGY_GENERATED:
								sprintf(szTmp, "%.3f kWh", meteroffset + (dvalue / divider));
								root["result"][ii]["Data"] = szTmp;
								root["result"][ii]["Counter"] = szTmp;
								break;
							case MTYPE_GAS:
								sprintf(szTmp, "%.3f m3", meteroffset + (dvalue / divider));
								root["result"][ii]["Data"] = szTmp;
								root["result"][ii]["Counter"] = szTmp;
								break;
							case MTYPE_WATER:
								sprintf(szTmp, "%.3f m3", meteroffset + (dvalue / divider));
								root["result"][ii]["Data"] = szTmp;
								root["result"][ii]["Counter"] = szTmp;
								break;
							case MTYPE_COUNTER:
								sprintf(szTmp, "%.10g", meteroffset + (dvalue / divider));
								if (!ValueUnits.empty())
								{
									strcat(szTmp, " ");
									strcat(szTmp, ValueUnits.c_str());
								}
								root["result"][ii]["Data"] = szTmp;
								root["result"][ii]["Counter"] = szTmp;
								break;
							default:
								root["result"][ii]["Data"] = "?";
								root["result"][ii]["Counter"] = "?";
								break;
							}
						}
						else if (dSubType == sTypeManagedCounter)
						{
							std::string ValueQuantity = options["ValueQuantity"];
							std::string ValueUnits = options["ValueUnits"];
							if (ValueQuantity.empty())
							{
								ValueQuantity = "Custom";
							}

							float divider = m_sql.GetCounterDivider(int(metertype), int(dType), float(AddjValue2));

							std::vector<std::string> splitresults;
							StringSplit(sValue, ";", splitresults);
							double dvalue;
							if (splitresults.size() < 2)
							{
								dvalue = static_cast<double>(atof(sValue.c_str()));
							}
							else
							{
								dvalue = static_cast<double>(atof(splitresults[1].c_str()));
								if (dvalue < 0.0)
								{
									dvalue = static_cast<double>(atof(splitresults[0].c_str()));
								}
							}
							root["result"][ii]["SwitchTypeVal"] = metertype;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							root["result"][ii]["TypeImg"] = "counter";
							root["result"][ii]["ValueQuantity"] = ValueQuantity;
							root["result"][ii]["ValueUnits"] = ValueUnits;
							root["result"][ii]["Divider"] = divider;
							root["result"][ii]["ShowNotifications"] = false;
							double meteroffset = AddjValue;

							switch (metertype)
							{
							case MTYPE_ENERGY:
							case MTYPE_ENERGY_GENERATED:
								sprintf(szTmp, "%.3f kWh", meteroffset + (dvalue / divider));
								root["result"][ii]["Data"] = szTmp;
								root["result"][ii]["Counter"] = szTmp;
								break;
							case MTYPE_GAS:
								sprintf(szTmp, "%.3f m3", meteroffset + (dvalue / divider));
								root["result"][ii]["Data"] = szTmp;
								root["result"][ii]["Counter"] = szTmp;
								break;
							case MTYPE_WATER:
								sprintf(szTmp, "%.3f m3", meteroffset + (dvalue / divider));
								root["result"][ii]["Data"] = szTmp;
								root["result"][ii]["Counter"] = szTmp;
								break;
							case MTYPE_COUNTER:
								sprintf(szTmp, "%.10g", meteroffset + (dvalue / divider));
								if (!ValueUnits.empty())
								{
									strcat(szTmp, " ");
									strcat(szTmp, ValueUnits.c_str());
								}
								root["result"][ii]["Data"] = szTmp;
								root["result"][ii]["Counter"] = szTmp;
								break;
							default:
								root["result"][ii]["Data"] = "?";
								root["result"][ii]["Counter"] = "?";
								break;
							}
						}
					}
					else if (dType == pTypeLux)
					{
						sprintf(szTmp, "%.0f Lux", atof(sValue.c_str()));
						root["result"][ii]["Data"] = szTmp;
						root["result"][ii]["HaveTimeout"] = bHaveTimeout;
					}
					else if (dType == pTypeWEIGHT)
					{
						sprintf(szTmp, "%g %s", m_sql.m_weightscale * atof(sValue.c_str()), m_sql.m_weightsign.c_str());
						root["result"][ii]["Data"] = szTmp;
						root["result"][ii]["HaveTimeout"] = false;
						root["result"][ii]["SwitchTypeVal"] = (m_sql.m_weightsign == "kg") ? 0 : 1;
					}
					else if (dType == pTypeUsage)
					{
						if (dSubType == sTypeElectric)
						{
							sprintf(szData, "%g Watt", atof(sValue.c_str()));
							root["result"][ii]["Data"] = szData;
						}
						else
						{
							root["result"][ii]["Data"] = sValue;
						}
						root["result"][ii]["HaveTimeout"] = bHaveTimeout;
					}
					else if (dType == pTypeRFXSensor)
					{
						switch (dSubType)
						{
						case sTypeRFXSensorAD:
							sprintf(szData, "%d mV", atoi(sValue.c_str()));
							root["result"][ii]["TypeImg"] = "current";
							break;
						case sTypeRFXSensorVolt:
							sprintf(szData, "%d mV", atoi(sValue.c_str()));
							root["result"][ii]["TypeImg"] = "current";
							break;
						}
						root["result"][ii]["Data"] = szData;
						root["result"][ii]["HaveTimeout"] = bHaveTimeout;
					}
					else if (dType == pTypeRego6XXValue)
					{
						switch (dSubType)
						{
						case sTypeRego6XXStatus:
						{
							std::string lstatus = "On";

							if (atoi(sValue.c_str()) == 0)
							{
								lstatus = "Off";
							}
							root["result"][ii]["Status"] = lstatus;
							root["result"][ii]["HaveDimmer"] = false;
							root["result"][ii]["MaxDimLevel"] = 0;
							root["result"][ii]["HaveGroupCmd"] = false;
							root["result"][ii]["SwitchTypeVal"] = STYPE_OnOff;
							root["result"][ii]["SwitchType"] = Switch_Type_Desc(STYPE_OnOff);
							sprintf(szData, "%d", atoi(sValue.c_str()));
							root["result"][ii]["Data"] = szData;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
							root["result"][ii]["StrParam1"] = strParam1;
							root["result"][ii]["StrParam2"] = strParam2;
							root["result"][ii]["Protected"] = (iProtected != 0);

							if (!CustomImage)
								root["result"][ii]["Image"] = "Light";
							root["result"][ii]["TypeImg"] = "utility";

							uint64_t camIDX = m_mainworker.m_cameras.IsDevSceneInCamera(0, sd[0]);
							root["result"][ii]["UsedByCamera"] = (camIDX != 0) ? true : false;
							if (camIDX != 0)
							{
								std::stringstream scidx;
								scidx << camIDX;
								root["result"][ii]["CameraIdx"] = scidx.str();
								root["result"][ii]["CameraAspect"] = m_mainworker.m_cameras.GetCameraAspectRatio(scidx.str());
							}

							root["result"][ii]["Level"] = 0;
							root["result"][ii]["LevelInt"] = atoi(sValue.c_str());
						}
						break;
						case sTypeRego6XXCounter:
						{
							// get value of today
							time_t now = mytime(nullptr);
							struct tm ltime;
							localtime_r(&now, &ltime);
							char szDate[40];
							sprintf(szDate, "%04d-%02d-%02d", ltime.tm_year + 1900, ltime.tm_mon + 1, ltime.tm_mday);

							std::vector<std::vector<std::string>> result2;
							strcpy(szTmp, "0");
							result2 = m_sql.safe_query("SELECT MIN(Value), MAX(Value) FROM Meter WHERE (DeviceRowID='%q' AND Date>='%q')", sd[0].c_str(), szDate);
							if (!result2.empty())
							{
								std::vector<std::string> sd2 = result2[0];

								uint64_t total_min = std::stoull(sd2[0]);
								uint64_t total_max = std::stoull(sd2[1]);
								uint64_t total_real = total_max - total_min;

								sprintf(szTmp, "%" PRIu64, total_real);
							}
							root["result"][ii]["SwitchTypeVal"] = MTYPE_COUNTER;
							root["result"][ii]["Counter"] = sValue;
							root["result"][ii]["CounterToday"] = szTmp;
							root["result"][ii]["Data"] = sValue;
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
						}
						break;
						}
					}
#ifdef ENABLE_PYTHON
					if (pHardware != nullptr)
					{
						if (pHardware->HwdType == HTYPE_PythonPlugin)
						{
							Plugins::CPlugin* pPlugin = (Plugins::CPlugin*)pHardware;
							bHaveTimeout = pPlugin->HasNodeFailed(sd[1].c_str(), atoi(sd[2].c_str()));
							root["result"][ii]["HaveTimeout"] = bHaveTimeout;
						}
					}
#endif
					root["result"][ii]["Timers"] = (bHasTimers == true) ? "true" : "false";
					ii++;
				}
				catch (const std::exception& e)
				{
					_log.Log(LOG_ERROR, "GetJSonDevices: exception occurred : '%s'", e.what());
					continue;
				}
			}
		}

		void CWebServer::UploadFloorplanImage(WebEmSession& session, const request& req, std::string& redirect_uri)
		{
			Json::Value root;
			root["title"] = "UploadFloorplanImage";
			root["status"] = "ERR";

			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string planname = request::findValue(&req, "planname");
			std::string scalefactor = request::findValue(&req, "scalefactor");
			std::string imagefile = request::findValue(&req, "imagefile");

			std::vector<std::vector<std::string>> result;
			m_sql.safe_query("INSERT INTO Floorplans ([Name],[ScaleFactor]) VALUES('%s','%s')", planname.c_str(), scalefactor.c_str());
			result = m_sql.safe_query("SELECT MAX(ID) FROM Floorplans");
			if (!result.empty())
			{
				if (!m_sql.safe_UpdateBlobInTableWithID("Floorplans", "Image", result[0][0], imagefile))
				{
					_log.Log(LOG_ERROR, "SQL: Problem inserting floorplan image into database! ");
				}
				else
					root["status"] = "OK";
			}
			redirect_uri = root.toStyledString();
		}

		void CWebServer::GetServiceWorker(WebEmSession& session, const request& req, reply& rep)
		{
			// Return the appcache file (dynamically generated)
			std::string sLine;
			std::string filename = szWWWFolder + "/service-worker.js";

			std::string response;

			std::ifstream is(filename.c_str());
			if (is)
			{
				while (!is.eof())
				{
					getline(is, sLine);
					if (!sLine.empty())
					{
						if (sLine.find("#BuildHash") != std::string::npos)
						{
							stdreplace(sLine, "#BuildHash", szAppHash);
						}
					}
					response += sLine + '\n';
				}
			}
			reply::set_content(&rep, response);
		}

		void CWebServer::GetFloorplanImage(WebEmSession& session, const request& req, reply& rep)
		{
			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
			{
				return;
			}
			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_queryBlob("SELECT Image FROM Floorplans WHERE ID=%d", atol(idx.c_str()));
			if (result.empty())
				return;
			reply::set_content(&rep, result[0][0].begin(), result[0][0].end());
			std::string oname = "floorplan";
			if (result[0][0].size() > 10)
			{
				if (result[0][0][0] == 'P')
					oname += ".png";
				else if (result[0][0][0] == -1)
					oname += ".jpg";
				else if (result[0][0][0] == 'B')
					oname += ".bmp";
				else if (result[0][0][0] == 'G')
					oname += ".gif";
				else if ((result[0][0][0] == '<') && (result[0][0][1] == 's') && (result[0][0][2] == 'v') && (result[0][0][3] == 'g'))
					oname += ".svg";
				else if (result[0][0].find("<svg") != std::string::npos) // some SVG's start with <xml
					oname += ".svg";
			}
			reply::add_header_attachment(&rep, oname);
		}

		void CWebServer::GetDatabaseBackup(WebEmSession& session, const request& req, reply& rep)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			time_t now = mytime(nullptr);
			Json::Value backupInfo;

			backupInfo["type"] = "Web";
#ifdef WIN32
			backupInfo["location"] = szUserDataFolder + "backup.db";
#else
			backupInfo["location"] = "/tmp/backup.db";
#endif
			if (m_sql.BackupDatabase(backupInfo["location"].asString()))
			{
				std::string szAttachmentName = "domoticz.db";
				std::string szVar;
				if (m_sql.GetPreferencesVar("Title", szVar))
				{
					stdreplace(szVar, " ", "_");
					stdreplace(szVar, "/", "_");
					stdreplace(szVar, "\\", "_");
					if (!szVar.empty())
					{
						szAttachmentName = szVar + ".db";
					}
				}
				reply::set_download_file(&rep, backupInfo["location"].asString(), szAttachmentName);
				backupInfo["duration"] = difftime(mytime(nullptr), now);
				m_mainworker.m_notificationsystem.Notify(Notification::DZ_BACKUP_DONE, Notification::STATUS_INFO, JSonToRawString(backupInfo));
			}
		}

		void CWebServer::Cmd_DeleteDevice(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string idx = CURLEncode::URLDecode(request::findValue(&req, "idx"));
			if (idx.empty())
				return;

			root["status"] = "OK";
			root["title"] = "DeleteDevice";
			m_sql.DeleteDevices(idx);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_AddScene(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string name = HTMLSanitizer::Sanitize(request::findValue(&req, "name"));
			name = HTMLSanitizer::Sanitize(name);
			if (name.empty())
			{
				root["status"] = "ERR";
				root["message"] = "No Scene Name specified!";
				return;
			}
			std::string stype = request::findValue(&req, "scenetype");
			if (stype.empty())
			{
				root["status"] = "ERR";
				root["message"] = "No Scene Type specified!";
				return;
			}
			if (m_sql.DoesSceneByNameExits(name) == true)
			{
				root["status"] = "ERR";
				root["message"] = "A Scene with this Name already Exits!";
				return;
			}
			root["status"] = "OK";
			root["title"] = "AddScene";
			m_sql.safe_query("INSERT INTO Scenes (Name,SceneType) VALUES ('%q',%d)", name.c_str(), atoi(stype.c_str()));
			if (m_sql.m_bEnableEventSystem)
			{
				m_mainworker.m_eventsystem.GetCurrentScenesGroups();
			}
		}

		void CWebServer::Cmd_DeleteScene(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string idx = CURLEncode::URLDecode(request::findValue(&req, "idx"));
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "DeleteScene";
			m_sql.DeleteScenes(idx);
		}

		void CWebServer::Cmd_UpdateScene(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			std::string name = HTMLSanitizer::Sanitize(request::findValue(&req, "name"));
			std::string description = HTMLSanitizer::Sanitize(request::findValue(&req, "description"));

			name = HTMLSanitizer::Sanitize(name);
			description = HTMLSanitizer::Sanitize(description);

			if ((idx.empty()) || (name.empty()))
				return;
			std::string stype = request::findValue(&req, "scenetype");
			if (stype.empty())
			{
				root["status"] = "ERR";
				root["message"] = "No Scene Type specified!";
				return;
			}
			std::string tmpstr = request::findValue(&req, "protected");
			int iProtected = (tmpstr == "true") ? 1 : 0;

			std::string onaction = base64_decode(request::findValue(&req, "onaction"));
			std::string offaction = base64_decode(request::findValue(&req, "offaction"));

			root["status"] = "OK";
			root["title"] = "UpdateScene";
			m_sql.safe_query("UPDATE Scenes SET Name='%q', Description='%q', SceneType=%d, Protected=%d, OnAction='%q', OffAction='%q' WHERE (ID == '%q')", name.c_str(),
				description.c_str(), atoi(stype.c_str()), iProtected, onaction.c_str(), offaction.c_str(), idx.c_str());
			uint64_t ullidx = std::stoull(idx);
			m_mainworker.m_eventsystem.WWWUpdateSingleState(ullidx, name, m_mainworker.m_eventsystem.REASON_SCENEGROUP);
		}

		bool compareIconsByName(const http::server::CWebServer::_tCustomIcon& a, const http::server::CWebServer::_tCustomIcon& b)
		{
			return a.Title < b.Title;
		}

		void CWebServer::Cmd_CustomLightIcons(WebEmSession& session, const request& req, Json::Value& root)
		{
			int ii = 0;

			std::vector<_tCustomIcon> temp_custom_light_icons = m_custom_light_icons;
			// Sort by name
			std::sort(temp_custom_light_icons.begin(), temp_custom_light_icons.end(), compareIconsByName);

			root["title"] = "CustomLightIcons";
			for (const auto& icon : temp_custom_light_icons)
			{
				root["result"][ii]["idx"] = icon.idx;
				root["result"][ii]["imageSrc"] = icon.RootFile;
				root["result"][ii]["text"] = icon.Title;
				root["result"][ii]["description"] = icon.Description;
				ii++;
			}
			root["status"] = "OK";
		}

		void CWebServer::Cmd_GetPlans(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "OK";
			root["title"] = "getplans";

			std::string sDisplayHidden = request::findValue(&req, "displayhidden");
			bool bDisplayHidden = (sDisplayHidden == "1");

			std::vector<std::vector<std::string>> result, result2;
			result = m_sql.safe_query("SELECT ID, Name, [Order] FROM Plans ORDER BY [Order]");
			if (!result.empty())
			{
				int ii = 0;
				for (const auto& sd : result)
				{
					std::string Name = sd[1];
					bool bIsHidden = (Name[0] == '$');

					if ((bDisplayHidden) || (!bIsHidden))
					{
						root["result"][ii]["idx"] = sd[0];
						root["result"][ii]["Name"] = Name;
						root["result"][ii]["Order"] = sd[2];

						unsigned int totDevices = 0;

						result2 = m_sql.safe_query("SELECT COUNT(*) FROM DeviceToPlansMap WHERE (PlanID=='%q')", sd[0].c_str());
						if (!result2.empty())
						{
							totDevices = (unsigned int)atoi(result2[0][0].c_str());
						}
						root["result"][ii]["Devices"] = totDevices;

						ii++;
					}
				}
			}
		}

		void CWebServer::Cmd_GetFloorPlans(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "OK";
			root["title"] = "getfloorplans";

			std::vector<std::vector<std::string>> result, result2, result3;

			result = m_sql.safe_query("SELECT Key, nValue, sValue FROM Preferences WHERE Key LIKE 'Floorplan%%'");
			if (result.empty())
				return;

			for (const auto& sd : result)
			{
				std::string Key = sd[0];
				int nValue = atoi(sd[1].c_str());
				std::string sValue = sd[2];

				if (Key == "FloorplanPopupDelay")
				{
					root["PopupDelay"] = nValue;
				}
				if (Key == "FloorplanFullscreenMode")
				{
					root["FullscreenMode"] = nValue;
				}
				if (Key == "FloorplanAnimateZoom")
				{
					root["AnimateZoom"] = nValue;
				}
				if (Key == "FloorplanShowSensorValues")
				{
					root["ShowSensorValues"] = nValue;
				}
				if (Key == "FloorplanShowSwitchValues")
				{
					root["ShowSwitchValues"] = nValue;
				}
				if (Key == "FloorplanShowSceneNames")
				{
					root["ShowSceneNames"] = nValue;
				}
				if (Key == "FloorplanRoomColour")
				{
					root["RoomColour"] = sValue;
				}
				if (Key == "FloorplanActiveOpacity")
				{
					root["ActiveRoomOpacity"] = nValue;
				}
				if (Key == "FloorplanInactiveOpacity")
				{
					root["InactiveRoomOpacity"] = nValue;
				}
			}

			result2 = m_sql.safe_query("SELECT ID, Name, ScaleFactor, [Order] FROM Floorplans ORDER BY [Order]");
			if (!result2.empty())
			{
				int ii = 0;
				for (const auto& sd : result2)
				{
					root["result"][ii]["idx"] = sd[0];
					root["result"][ii]["Name"] = sd[1];
					std::string ImageURL = "images/floorplans/plan?idx=" + sd[0];
					root["result"][ii]["Image"] = ImageURL;
					root["result"][ii]["ScaleFactor"] = sd[2];
					root["result"][ii]["Order"] = sd[3];

					unsigned int totPlans = 0;

					result3 = m_sql.safe_query("SELECT COUNT(*) FROM Plans WHERE (FloorplanID=='%q')", sd[0].c_str());
					if (!result3.empty())
					{
						totPlans = (unsigned int)atoi(result3[0][0].c_str());
					}
					root["result"][ii]["Plans"] = totPlans;

					ii++;
				}
			}
		}

		void CWebServer::Cmd_GetScenes(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "OK";
			root["title"] = "getscenes";
			root["AllowWidgetOrdering"] = m_sql.m_bAllowWidgetOrdering;

			std::string sDisplayHidden = request::findValue(&req, "displayhidden");
			bool bDisplayHidden = (sDisplayHidden == "1");

			std::string sLastUpdate = request::findValue(&req, "lastupdate");

			std::string rid = request::findValue(&req, "rid");

			time_t LastUpdate = 0;
			if (!sLastUpdate.empty())
			{
				std::stringstream sstr;
				sstr << sLastUpdate;
				sstr >> LastUpdate;
			}

			time_t now = mytime(nullptr);
			struct tm tm1;
			localtime_r(&now, &tm1);
			struct tm tLastUpdate;
			localtime_r(&now, &tLastUpdate);

			root["ActTime"] = static_cast<int>(now);

			std::vector<std::vector<std::string>> result, result2;
			std::string szQuery = "SELECT ID, Name, Activators, Favorite, nValue, SceneType, LastUpdate, Protected, OnAction, OffAction, Description FROM Scenes";
			if (!rid.empty())
				szQuery += " WHERE (ID == " + rid + ")";
			szQuery += " ORDER BY [Order]";
			result = m_sql.safe_query(szQuery.c_str());
			if (!result.empty())
			{
				int ii = 0;
				for (const auto& sd : result)
				{
					std::string sName = sd[1];
					if ((bDisplayHidden == false) && (sName[0] == '$'))
						continue;

					std::string sLastUpdate = sd[6];
					if (LastUpdate != 0)
					{
						time_t cLastUpdate;
						ParseSQLdatetime(cLastUpdate, tLastUpdate, sLastUpdate, tm1.tm_isdst);
						if (cLastUpdate <= LastUpdate)
							continue;
					}

					unsigned char nValue = atoi(sd[4].c_str());
					unsigned char scenetype = atoi(sd[5].c_str());
					int iProtected = atoi(sd[7].c_str());

					std::string onaction = base64_encode(sd[8]);
					std::string offaction = base64_encode(sd[9]);

					root["result"][ii]["idx"] = sd[0];
					root["result"][ii]["Name"] = sName;
					root["result"][ii]["Description"] = sd[10];
					root["result"][ii]["Favorite"] = atoi(sd[3].c_str());
					root["result"][ii]["Protected"] = (iProtected != 0);
					root["result"][ii]["OnAction"] = onaction;
					root["result"][ii]["OffAction"] = offaction;

					if (scenetype == 0)
					{
						root["result"][ii]["Type"] = "Scene";
					}
					else
					{
						root["result"][ii]["Type"] = "Group";
					}

					root["result"][ii]["LastUpdate"] = sLastUpdate;

					if (nValue == 0)
						root["result"][ii]["Status"] = "Off";
					else if (nValue == 1)
						root["result"][ii]["Status"] = "On";
					else
						root["result"][ii]["Status"] = "Mixed";
					root["result"][ii]["Timers"] = (m_sql.HasSceneTimers(sd[0]) == true) ? "true" : "false";
					uint64_t camIDX = m_mainworker.m_cameras.IsDevSceneInCamera(1, sd[0]);
					root["result"][ii]["UsedByCamera"] = (camIDX != 0) ? true : false;
					if (camIDX != 0)
					{
						std::stringstream scidx;
						scidx << camIDX;
						root["result"][ii]["CameraIdx"] = scidx.str();
						root["result"][ii]["CameraAspect"] = m_mainworker.m_cameras.GetCameraAspectRatio(scidx.str());
					}
					ii++;
				}
			}
			if (!m_mainworker.m_LastSunriseSet.empty())
			{
				std::vector<std::string> strarray;
				StringSplit(m_mainworker.m_LastSunriseSet, ";", strarray);
				if (strarray.size() == 10)
				{
					char szTmp[100];
					// strftime(szTmp, 80, "%b %d %Y %X", &tm1);
					strftime(szTmp, 80, "%Y-%m-%d %X", &tm1);
					root["ServerTime"] = szTmp;
					root["Sunrise"] = strarray[0];
					root["Sunset"] = strarray[1];
					root["SunAtSouth"] = strarray[2];
					root["CivTwilightStart"] = strarray[3];
					root["CivTwilightEnd"] = strarray[4];
					root["NautTwilightStart"] = strarray[5];
					root["NautTwilightEnd"] = strarray[6];
					root["AstrTwilightStart"] = strarray[7];
					root["AstrTwilightEnd"] = strarray[8];
					root["DayLength"] = strarray[9];
				}
			}
		}

		void CWebServer::Cmd_GetHardware(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["title"] = "gethardware";

			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT ID, Name, Enabled, Type, Address, Port, SerialPort, Username, Password, Extra, Mode1, Mode2, Mode3, Mode4, Mode5, Mode6, DataTimeout, "
				"LogLevel FROM Hardware ORDER BY ID ASC");
			if (!result.empty())
			{
				int ii = 0;
				for (const auto& sd : result)
				{
					_eHardwareTypes hType = (_eHardwareTypes)atoi(sd[3].c_str());
					if (hType == HTYPE_DomoticzInternal)
						continue;
					root["result"][ii]["idx"] = sd[0];
					root["result"][ii]["Name"] = sd[1];
					root["result"][ii]["Enabled"] = (sd[2] == "1") ? "true" : "false";
					root["result"][ii]["Type"] = hType;
					root["result"][ii]["Address"] = sd[4];
					root["result"][ii]["Port"] = atoi(sd[5].c_str());
					root["result"][ii]["SerialPort"] = sd[6];
					root["result"][ii]["Username"] = sd[7];
					root["result"][ii]["Password"] = sd[8];
					root["result"][ii]["Extra"] = sd[9];

					if (hType == HTYPE_PythonPlugin)
					{
						root["result"][ii]["Mode1"] = sd[10]; // Plugins can have non-numeric values in the Mode fields
						root["result"][ii]["Mode2"] = sd[11];
						root["result"][ii]["Mode3"] = sd[12];
						root["result"][ii]["Mode4"] = sd[13];
						root["result"][ii]["Mode5"] = sd[14];
						root["result"][ii]["Mode6"] = sd[15];
					}
					else
					{
						root["result"][ii]["Mode1"] = atoi(sd[10].c_str());
						root["result"][ii]["Mode2"] = atoi(sd[11].c_str());
						root["result"][ii]["Mode3"] = atoi(sd[12].c_str());
						root["result"][ii]["Mode4"] = atoi(sd[13].c_str());
						root["result"][ii]["Mode5"] = atoi(sd[14].c_str());
						root["result"][ii]["Mode6"] = atoi(sd[15].c_str());
					}
					root["result"][ii]["DataTimeout"] = atoi(sd[16].c_str());
					root["result"][ii]["LogLevel"] = atoi(sd[17].c_str());

					CDomoticzHardwareBase* pHardware = m_mainworker.GetHardware(atoi(sd[0].c_str()));
					if (pHardware != nullptr)
					{
						if ((pHardware->HwdType == HTYPE_RFXtrx315) || (pHardware->HwdType == HTYPE_RFXtrx433) || (pHardware->HwdType == HTYPE_RFXtrx868) ||
							(pHardware->HwdType == HTYPE_RFXLAN))
						{
							CRFXBase* pMyHardware = dynamic_cast<CRFXBase*>(pHardware);
							if (!pMyHardware->m_Version.empty())
								root["result"][ii]["version"] = pMyHardware->m_Version;
							else
								root["result"][ii]["version"] = sd[11];
							root["result"][ii]["noiselvl"] = pMyHardware->m_NoiseLevel;
						}
						else if ((pHardware->HwdType == HTYPE_MySensorsUSB) || (pHardware->HwdType == HTYPE_MySensorsTCP) || (pHardware->HwdType == HTYPE_MySensorsMQTT))
						{
							MySensorsBase* pMyHardware = dynamic_cast<MySensorsBase*>(pHardware);
							root["result"][ii]["version"] = pMyHardware->GetGatewayVersion();
						}
						else if ((pHardware->HwdType == HTYPE_OpenThermGateway) || (pHardware->HwdType == HTYPE_OpenThermGatewayTCP))
						{
							OTGWBase* pMyHardware = dynamic_cast<OTGWBase*>(pHardware);
							root["result"][ii]["version"] = pMyHardware->m_Version;
						}
						else if ((pHardware->HwdType == HTYPE_RFLINKUSB) || (pHardware->HwdType == HTYPE_RFLINKTCP))
						{
							CRFLinkBase* pMyHardware = dynamic_cast<CRFLinkBase*>(pHardware);
							root["result"][ii]["version"] = pMyHardware->m_Version;
						}
						else if (pHardware->HwdType == HTYPE_EnphaseAPI)
						{
							EnphaseAPI* pMyHardware = dynamic_cast<EnphaseAPI*>(pHardware);
							root["result"][ii]["version"] = pMyHardware->m_szSoftwareVersion;
						}
						else if (pHardware->HwdType == HTYPE_AlfenEveCharger)
						{
							AlfenEve* pMyHardware = dynamic_cast<AlfenEve*>(pHardware);
							root["result"][ii]["version"] = pMyHardware->m_szSoftwareVersion;
						}
					}
					ii++;
				}
			}
			root["status"] = "OK";
		}

		void CWebServer::Cmd_GetDevices(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string rfilter = request::findValue(&req, "filter");
			std::string order = request::findValue(&req, "order");
			std::string rused = request::findValue(&req, "used");
			std::string rid = request::findValue(&req, "rid");
			std::string planid = request::findValue(&req, "plan");
			std::string floorid = request::findValue(&req, "floor");
			std::string sDisplayHidden = request::findValue(&req, "displayhidden");
			std::string sFetchFavorites = request::findValue(&req, "favorite");
			std::string sDisplayDisabled = request::findValue(&req, "displaydisabled");
			bool bDisplayHidden = (sDisplayHidden == "1");
			bool bFetchFavorites = (sFetchFavorites == "1");

			int HideDisabledHardwareSensors = 0;
			m_sql.GetPreferencesVar("HideDisabledHardwareSensors", HideDisabledHardwareSensors);
			bool bDisabledDisabled = (HideDisabledHardwareSensors == 0);
			if (sDisplayDisabled == "1")
				bDisabledDisabled = true;

			std::string sLastUpdate = request::findValue(&req, "lastupdate");
			std::string hwidx = request::findValue(&req, "hwidx"); // OTO

			time_t LastUpdate = 0;
			if (!sLastUpdate.empty())
			{
				std::stringstream sstr;
				sstr << sLastUpdate;
				sstr >> LastUpdate;
			}

			root["status"] = "OK";
			root["title"] = "Devices";
			root["app_version"] = szAppVersion;
			GetJSonDevices(root, rused, rfilter, order, rid, planid, floorid, bDisplayHidden, bDisabledDisabled, bFetchFavorites, LastUpdate, session.username, hwidx);
		}

		void CWebServer::Cmd_GetUsers(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "ERR";
			root["title"] = "Users";

			if (session.rights != URIGHTS_ADMIN)
				return;

			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT ID, Active, Username, Password, Rights, RemoteSharing, TabsEnabled FROM USERS ORDER BY ID ASC");
			if (!result.empty())
			{
				int ii = 0;
				for (const auto& sd : result)
				{
					root["result"][ii]["idx"] = sd[0];
					root["result"][ii]["Enabled"] = (sd[1] == "1") ? "true" : "false";
					root["result"][ii]["Username"] = base64_decode(sd[2]);
					root["result"][ii]["Password"] = sd[3];
					root["result"][ii]["Rights"] = atoi(sd[4].c_str());
					root["result"][ii]["RemoteSharing"] = atoi(sd[5].c_str());
					root["result"][ii]["TabsEnabled"] = atoi(sd[6].c_str());
					ii++;
				}
				root["status"] = "OK";
			}
		}

		void CWebServer::Cmd_GetMobiles(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "ERR";
			root["title"] = "Mobiles";

			if (session.rights != URIGHTS_ADMIN)
				return;

			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT ID, Active, Name, UUID, LastUpdate, DeviceType FROM MobileDevices ORDER BY Name COLLATE NOCASE ASC");
			if (!result.empty())
			{
				int ii = 0;
				for (const auto& sd : result)
				{
					root["result"][ii]["idx"] = sd[0];
					root["result"][ii]["Enabled"] = (sd[1] == "1") ? "true" : "false";
					root["result"][ii]["Name"] = sd[2];
					root["result"][ii]["UUID"] = sd[3];
					root["result"][ii]["LastUpdate"] = sd[4];
					root["result"][ii]["DeviceType"] = sd[5];
					ii++;
				}
			}
			root["status"] = "OK";
		}

		void CWebServer::Cmd_SetSetpoint(WebEmSession& session, const request& req, Json::Value& root)
		{
			bool bHaveUser = (!session.username.empty());
			int iUser = -1;
			int urights = 3;
			if (bHaveUser)
			{
				iUser = FindUser(session.username.c_str());
				if (iUser != -1)
				{
					urights = static_cast<int>(m_users[iUser].userrights);
				}
			}
			if (urights < 1)
				return;

			std::string idx = request::findValue(&req, "idx");
			std::string setpoint = request::findValue(&req, "setpoint");
			if ((idx.empty()) || (setpoint.empty()))
				return;
			root["status"] = "OK";
			root["title"] = "SetSetpoint";
			if (iUser != -1)
			{
				_log.Log(LOG_STATUS, "User: %s initiated a SetPoint command", m_users[iUser].Username.c_str());
			}
			m_mainworker.SetSetPoint(idx, static_cast<float>(atof(setpoint.c_str())));
		}

		void CWebServer::Cmd_GetSceneActivations(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;

			root["status"] = "OK";
			root["title"] = "GetSceneActivations";

			std::vector<std::vector<std::string>> result, result2;
			result = m_sql.safe_query("SELECT Activators, SceneType FROM Scenes WHERE (ID==%q)", idx.c_str());
			if (result.empty())
				return;
			int ii = 0;
			std::string Activators = result[0][0];
			int SceneType = atoi(result[0][1].c_str());
			if (!Activators.empty())
			{
				// Get Activator device names
				std::vector<std::string> arrayActivators;
				StringSplit(Activators, ";", arrayActivators);
				for (const auto& sCodeCmd : arrayActivators)
				{
					std::vector<std::string> arrayCode;
					StringSplit(sCodeCmd, ":", arrayCode);

					std::string sID = arrayCode[0];
					int sCode = 0;
					if (arrayCode.size() == 2)
					{
						sCode = atoi(arrayCode[1].c_str());
					}

					result2 = m_sql.safe_query("SELECT Name, [Type], SubType, SwitchType FROM DeviceStatus WHERE (ID==%q)", sID.c_str());
					if (!result2.empty())
					{
						std::vector<std::string> sd = result2[0];
						std::string lstatus = "-";
						if ((SceneType == 0) && (arrayCode.size() == 2))
						{
							unsigned char devType = (unsigned char)atoi(sd[1].c_str());
							unsigned char subType = (unsigned char)atoi(sd[2].c_str());
							_eSwitchType switchtype = (_eSwitchType)atoi(sd[3].c_str());
							int nValue = sCode;
							std::string sValue;
							int llevel = 0;
							bool bHaveDimmer = false;
							bool bHaveGroupCmd = false;
							int maxDimLevel = 0;
							GetLightStatus(devType, subType, switchtype, nValue, sValue, lstatus, llevel, bHaveDimmer, maxDimLevel, bHaveGroupCmd);
						}
						uint64_t dID = std::stoull(sID);
						root["result"][ii]["idx"] = Json::Value::UInt64(dID);
						root["result"][ii]["name"] = sd[0];
						root["result"][ii]["code"] = sCode;
						root["result"][ii]["codestr"] = lstatus;
						ii++;
					}
				}
			}
		}

		void CWebServer::Cmd_AddSceneCode(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string sceneidx = request::findValue(&req, "sceneidx");
			std::string idx = request::findValue(&req, "idx");
			std::string cmnd = request::findValue(&req, "cmnd");
			if ((sceneidx.empty()) || (idx.empty()) || (cmnd.empty()))
				return;
			root["status"] = "OK";
			root["title"] = "AddSceneCode";

			// First check if we do not already have this device as activation code
			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT Activators, SceneType FROM Scenes WHERE (ID==%q)", sceneidx.c_str());
			if (result.empty())
				return;
			std::string Activators = result[0][0];
			unsigned char scenetype = atoi(result[0][1].c_str());

			if (!Activators.empty())
			{
				// Get Activator device names
				std::vector<std::string> arrayActivators;
				StringSplit(Activators, ";", arrayActivators);
				for (const auto& sCodeCmd : arrayActivators)
				{
					std::vector<std::string> arrayCode;
					StringSplit(sCodeCmd, ":", arrayCode);

					std::string sID = arrayCode[0];
					std::string sCode;
					if (arrayCode.size() == 2)
					{
						sCode = arrayCode[1];
					}

					if (sID == idx)
					{
						if (scenetype == 1)
							return; // Group does not work with separate codes, so already there
						if (sCode == cmnd)
							return; // same code, already there!
					}
				}
			}
			if (!Activators.empty())
				Activators += ";";
			Activators += idx;
			if (scenetype == 0)
			{
				Activators += ":" + cmnd;
			}
			m_sql.safe_query("UPDATE Scenes SET Activators='%q' WHERE (ID==%q)", Activators.c_str(), sceneidx.c_str());
		}

		void CWebServer::Cmd_RemoveSceneCode(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string sceneidx = request::findValue(&req, "sceneidx");
			std::string idx = request::findValue(&req, "idx");
			std::string code = request::findValue(&req, "code");
			if ((idx.empty()) || (sceneidx.empty()) || (code.empty()))
				return;
			root["status"] = "OK";
			root["title"] = "RemoveSceneCode";

			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT Activators, SceneType FROM Scenes WHERE (ID==%q)", sceneidx.c_str());
			if (result.empty())
				return;
			std::string Activators = result[0][0];
			int SceneType = atoi(result[0][1].c_str());
			if (!Activators.empty())
			{
				// Get Activator device names
				std::vector<std::string> arrayActivators;
				StringSplit(Activators, ";", arrayActivators);
				std::string newActivation;
				for (const auto& sCodeCmd : arrayActivators)
				{
					std::vector<std::string> arrayCode;
					StringSplit(sCodeCmd, ":", arrayCode);

					std::string sID = arrayCode[0];
					std::string sCode;
					if (arrayCode.size() == 2)
					{
						sCode = arrayCode[1];
					}
					bool bFound = false;
					if (sID == idx)
					{
						if ((SceneType == 1) || (sCode.empty()))
						{
							bFound = true;
						}
						else
						{
							// Also check the code
							bFound = (sCode == code);
						}
					}
					if (!bFound)
					{
						if (!newActivation.empty())
							newActivation += ";";
						newActivation += sID;
						if ((SceneType == 0) && (!sCode.empty()))
						{
							newActivation += ":" + sCode;
						}
					}
				}
				if (Activators != newActivation)
				{
					m_sql.safe_query("UPDATE Scenes SET Activators='%q' WHERE (ID==%q)", newActivation.c_str(), sceneidx.c_str());
				}
			}
		}

		void CWebServer::Cmd_ClearSceneCodes(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string sceneidx = request::findValue(&req, "sceneidx");
			if (sceneidx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "ClearSceneCode";

			m_sql.safe_query("UPDATE Scenes SET Activators='' WHERE (ID==%q)", sceneidx.c_str());
		}

		void CWebServer::Cmd_GetSerialDevices(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "OK";
			root["title"] = "GetSerialDevices";

			bool bUseDirectPath = false;
			std::vector<std::string> serialports = GetSerialPorts(bUseDirectPath);
			int ii = 0;
			for (const auto& port : serialports)
			{
				root["result"][ii]["name"] = port;
				root["result"][ii]["value"] = ii;
				ii++;
			}
		}

		void CWebServer::Cmd_GetDevicesList(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "OK";
			root["title"] = "GetDevicesList";
			int ii = 0;
			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT ID, Name, Type, SubType FROM DeviceStatus WHERE (Used == 1) ORDER BY Name COLLATE NOCASE ASC");
			if (!result.empty())
			{
				for (const auto& sd : result)
				{
					root["result"][ii]["idx"] = sd[0];
					root["result"][ii]["name"] = sd[1];
					root["result"][ii]["name_type"] = std_format("%s (%s/%s)",
						sd[1].c_str(),
						RFX_Type_Desc(std::stoi(sd[2]), 1),
						RFX_Type_SubType_Desc(std::stoi(sd[2]), std::stoi(sd[3]))
					);
					//root["result"][ii]["Type"] = RFX_Type_Desc(std::stoi(sd[2]), 1);
					//root["result"][ii]["SubType"] = RFX_Type_SubType_Desc(std::stoi(sd[2]), std::stoi(sd[3]));
					ii++;
				}
			}
		}

		void CWebServer::Cmd_UploadCustomIcon(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["title"] = "UploadCustomIcon";
			// Only admin user allowed
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			std::string zipfile = request::findValue(&req, "file");
			if (!zipfile.empty())
			{
				std::string ErrorMessage;
				bool bOK = m_sql.InsertCustomIconFromZip(zipfile, ErrorMessage);
				if (bOK)
				{
					root["status"] = "OK";
				}
				else
				{
					root["error"] = ErrorMessage;
				}
			}
		}

		void CWebServer::Cmd_GetCustomIconSet(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "OK";
			root["title"] = "GetCustomIconSet";
			int ii = 0;
			for (const auto& icon : m_custom_light_icons)
			{
				if (icon.idx >= 100)
				{
					std::string IconFile16 = "images/" + icon.RootFile + ".png";
					std::string IconFile48On = "images/" + icon.RootFile + "48_On.png";
					std::string IconFile48Off = "images/" + icon.RootFile + "48_Off.png";

					root["result"][ii]["idx"] = icon.idx - 100;
					root["result"][ii]["Title"] = icon.Title;
					root["result"][ii]["Description"] = icon.Description;
					root["result"][ii]["IconFile16"] = IconFile16;
					root["result"][ii]["IconFile48On"] = IconFile48On;
					root["result"][ii]["IconFile48Off"] = IconFile48Off;
					ii++;
				}
			}
		}

		void CWebServer::Cmd_DeleteCustomIcon(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string sidx = request::findValue(&req, "idx");
			if (sidx.empty())
				return;
			int idx = atoi(sidx.c_str());
			root["status"] = "OK";
			root["title"] = "DeleteCustomIcon";

			m_sql.safe_query("DELETE FROM CustomImages WHERE (ID == %d)", idx);

			// Delete icons file from disk
			for (const auto& icon : m_custom_light_icons)
			{
				if (icon.idx == idx + 100)
				{
					std::string IconFile16 = szWWWFolder + "/images/" + icon.RootFile + ".png";
					std::string IconFile48On = szWWWFolder + "/images/" + icon.RootFile + "48_On.png";
					std::string IconFile48Off = szWWWFolder + "/images/" + icon.RootFile + "48_Off.png";
					std::remove(IconFile16.c_str());
					std::remove(IconFile48On.c_str());
					std::remove(IconFile48Off.c_str());
					break;
				}
			}
			ReloadCustomSwitchIcons();
		}

		void CWebServer::Cmd_UpdateCustomIcon(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string sidx = request::findValue(&req, "idx");
			std::string sname = HTMLSanitizer::Sanitize(request::findValue(&req, "name"));
			std::string sdescription = HTMLSanitizer::Sanitize(request::findValue(&req, "description"));
			if ((sidx.empty()) || (sname.empty()) || (sdescription.empty()))
				return;

			int idx = atoi(sidx.c_str());
			root["status"] = "OK";
			root["title"] = "UpdateCustomIcon";

			m_sql.safe_query("UPDATE CustomImages SET Name='%q', Description='%q' WHERE (ID == %d)", sname.c_str(), sdescription.c_str(), idx);
			ReloadCustomSwitchIcons();
		}

		void CWebServer::Cmd_RenameDevice(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string sidx = request::findValue(&req, "idx");
			std::string sname = HTMLSanitizer::Sanitize(request::findValue(&req, "name"));
			if ((sidx.empty()) || (sname.empty()))
				return;
			int idx = atoi(sidx.c_str());
			root["status"] = "OK";
			root["title"] = "RenameDevice";

			m_sql.safe_query("UPDATE DeviceStatus SET Name='%q' WHERE (ID == %d)", sname.c_str(), idx);
			uint64_t ullidx = std::stoull(sidx);
			m_mainworker.m_eventsystem.WWWUpdateSingleState(ullidx, sname, m_mainworker.m_eventsystem.REASON_DEVICE);

#ifdef ENABLE_PYTHON
			// Notify plugin framework about the change
			m_mainworker.m_pluginsystem.DeviceModified(idx);
#endif
		}

		void CWebServer::Cmd_RenameScene(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string sidx = request::findValue(&req, "idx");
			std::string sname = HTMLSanitizer::Sanitize(request::findValue(&req, "name"));
			if ((sidx.empty()) || (sname.empty()))
				return;
			int idx = atoi(sidx.c_str());
			root["status"] = "OK";
			root["title"] = "RenameScene";

			m_sql.safe_query("UPDATE Scenes SET Name='%q' WHERE (ID == %d)", sname.c_str(), idx);
			uint64_t ullidx = std::stoull(sidx);
			m_mainworker.m_eventsystem.WWWUpdateSingleState(ullidx, sname, m_mainworker.m_eventsystem.REASON_SCENEGROUP);
		}

		void CWebServer::Cmd_SetDeviceUsed(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string sIdx = request::findValue(&req, "idx");
			std::string sUsed = request::findValue(&req, "used");
			std::string sName = request::findValue(&req, "name");
			std::string sMainDeviceIdx = request::findValue(&req, "maindeviceidx");
			if (sIdx.empty() || sUsed.empty())
				return;
			const int idx = atoi(sIdx.c_str());
			bool bIsUsed = (sUsed == "true");

			if (!sName.empty())
				m_sql.safe_query("UPDATE DeviceStatus SET Used=%d, Name='%q' WHERE (ID == %d)", bIsUsed ? 1 : 0, sName.c_str(), idx);
			else
				m_sql.safe_query("UPDATE DeviceStatus SET Used=%d WHERE (ID == %d)", bIsUsed ? 1 : 0, idx);

			root["status"] = "OK";
			root["title"] = "SetDeviceUsed";

			if ((!sMainDeviceIdx.empty()) && (sMainDeviceIdx != sIdx))
			{
				// this is a sub device for another light/switch
				// first check if it is not already a sub device
				auto result = m_sql.safe_query("SELECT ID FROM LightSubDevices WHERE (DeviceRowID=='%q') AND (ParentID =='%q')", sIdx.c_str(), sMainDeviceIdx.c_str());
				if (result.empty())
				{
					// no it is not, add it
					m_sql.safe_query("INSERT INTO LightSubDevices (DeviceRowID, ParentID) VALUES ('%q','%q')", sIdx.c_str(), sMainDeviceIdx.c_str());
				}
			}

			if (m_sql.m_bEnableEventSystem)
			{
				if (!bIsUsed)
					m_mainworker.m_eventsystem.RemoveSingleState(idx, m_mainworker.m_eventsystem.REASON_DEVICE);
				else
					m_mainworker.m_eventsystem.WWWUpdateSingleState(idx, sName, m_mainworker.m_eventsystem.REASON_DEVICE);
			}
#ifdef ENABLE_PYTHON
			// Notify plugin framework about the change
			m_mainworker.m_pluginsystem.DeviceModified(idx);
#endif
		}

		void CWebServer::Cmd_AddLogMessage(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string smessage = request::findValue(&req, "message");
			if (smessage.empty())
				return;
			root["title"] = "AddLogMessage";

			_eLogLevel logLevel = LOG_STATUS;
			std::string slevel = request::findValue(&req, "level");
			if (!slevel.empty())
			{
				if ((slevel == "1") || (slevel == "normal"))
					logLevel = LOG_NORM;
				else if ((slevel == "2") || (slevel == "status"))
					logLevel = LOG_STATUS;
				else if ((slevel == "4") || (slevel == "error"))
					logLevel = LOG_ERROR;
				else
				{
					root["status"] = "ERR";
					return;
				}
			}
			root["status"] = "OK";

			_log.Log(logLevel, "%s", smessage.c_str());
		}

		void CWebServer::Cmd_ClearShortLog(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			root["status"] = "OK";
			root["title"] = "ClearShortLog";

			_log.Log(LOG_STATUS, "Clearing Short Log...");

			m_sql.ClearShortLog();

			_log.Log(LOG_STATUS, "Short Log Cleared!");
		}

		void CWebServer::Cmd_VacuumDatabase(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			root["status"] = "OK";
			root["title"] = "VacuumDatabase";

			m_sql.VacuumDatabase();
		}

		void CWebServer::Cmd_AddMobileDevice(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string suuid = HTMLSanitizer::Sanitize(request::findValue(&req, "uuid"));
			std::string ssenderid = HTMLSanitizer::Sanitize(request::findValue(&req, "senderid"));
			std::string sname = HTMLSanitizer::Sanitize(request::findValue(&req, "name"));
			std::string sdevtype = HTMLSanitizer::Sanitize(request::findValue(&req, "devicetype"));
			std::string sactive = request::findValue(&req, "active");
			if ((suuid.empty()) || (ssenderid.empty()))
				return;
			root["status"] = "OK";
			root["title"] = "AddMobileDevice";

			if (sactive.empty())
				sactive = "1";
			int iActive = (sactive == "1") ? 1 : 0;

			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT ID, Name, DeviceType FROM MobileDevices WHERE (UUID=='%q')", suuid.c_str());
			if (result.empty())
			{
				// New
				m_sql.safe_query("INSERT INTO MobileDevices (Active,UUID,SenderID,Name,DeviceType) VALUES (%d,'%q','%q','%q','%q')", iActive, suuid.c_str(), ssenderid.c_str(),
					sname.c_str(), sdevtype.c_str());
			}
			else
			{
				// Update
				std::string sLastUpdate = TimeToString(nullptr, TF_DateTime);
				m_sql.safe_query("UPDATE MobileDevices SET Active=%d, SenderID='%q', LastUpdate='%q' WHERE (UUID == '%q')", iActive, ssenderid.c_str(),
					sLastUpdate.c_str(), suuid.c_str());

				std::string dname = result[0][1];
				std::string ddevtype = result[0][2];
				if (dname.empty() || ddevtype.empty())
				{
					m_sql.safe_query("UPDATE MobileDevices SET Name='%q', DeviceType='%q' WHERE (UUID == '%q')", sname.c_str(), sdevtype.c_str(), suuid.c_str());
				}
			}
		}

		void CWebServer::Cmd_UpdateMobileDevice(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			std::string sidx = request::findValue(&req, "idx");
			std::string enabled = request::findValue(&req, "enabled");
			std::string name = HTMLSanitizer::Sanitize(request::findValue(&req, "name"));

			if ((sidx.empty()) || (enabled.empty()) || (name.empty()))
				return;
			uint64_t idx = std::stoull(sidx);

			m_sql.safe_query("UPDATE MobileDevices SET Name='%q', Active=%d WHERE (ID==%" PRIu64 ")", name.c_str(), (enabled == "true") ? 1 : 0, idx);

			root["status"] = "OK";
			root["title"] = "UpdateMobile";
		}

		void CWebServer::Cmd_DeleteMobileDevice(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			std::string suuid = request::findValue(&req, "uuid");
			if (suuid.empty())
				return;
			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT ID FROM MobileDevices WHERE (UUID=='%q')", suuid.c_str());
			if (result.empty())
				return;
			m_sql.safe_query("DELETE FROM MobileDevices WHERE (UUID == '%q')", suuid.c_str());
			root["status"] = "OK";
			root["title"] = "DeleteMobileDevice";
		}

		void CWebServer::Cmd_GetTransfers(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "OK";
			root["title"] = "GetTransfers";

			uint64_t idx = 0;
			if (!request::findValue(&req, "idx").empty())
			{
				idx = std::stoull(request::findValue(&req, "idx"));
			}

			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT Type, SubType FROM DeviceStatus WHERE (ID==%" PRIu64 ")", idx);
			if (!result.empty())
			{
				int dType = atoi(result[0][0].c_str());
				if ((dType == pTypeTEMP) || (dType == pTypeTEMP_HUM) || (dType == pTypeTEMP_HUM_BARO))
				{
					//Allow a old Temperature device to be replaced by a new Temp+Hum or Temp+Hum+Baro
					//Or a Temp+Hum to a Temp+Hum or Temp+Hum+Baro
					if (dType == pTypeTEMP)
						result = m_sql.safe_query("SELECT ID, Name, Type FROM DeviceStatus WHERE (Type=='%d') || (Type=='%d') || (Type=='%d') AND (ID!=%" PRIu64 ")", pTypeTEMP, pTypeTEMP_HUM, pTypeTEMP_HUM_BARO, idx);
					else if (dType == pTypeTEMP_HUM)
						result = m_sql.safe_query("SELECT ID, Name, Type FROM DeviceStatus WHERE (Type=='%d') || (Type=='%d') AND (ID!=%" PRIu64 ")", pTypeTEMP_HUM, pTypeTEMP_HUM_BARO, idx);
					else
						result = m_sql.safe_query("SELECT ID, Name, Type FROM DeviceStatus WHERE (Type=='%q') AND (ID!=%" PRIu64 ")", result[0][0].c_str(), idx);
				}
				else
				{
					result = m_sql.safe_query("SELECT ID, Name FROM DeviceStatus WHERE (Type=='%q') AND (SubType=='%q') AND (ID!=%" PRIu64 ")", result[0][0].c_str(),
						result[0][1].c_str(), idx);
				}

				std::sort(std::begin(result), std::end(result), [](std::vector<std::string> a, std::vector<std::string> b) { return a[1] < b[1]; });

				int ii = 0;
				for (const auto& sd : result)
				{
					root["result"][ii]["idx"] = sd[0];
					root["result"][ii]["Name"] = sd[1];
					ii++;
				}
			}
		}

		// Will transfer Newest sensor log to OLD sensor,
		// then set the HardwareID/DeviceID/Unit/Name/Type/Subtype/Unit for the OLD sensor to the NEW sensor ID/Type/Subtype/Unit
		// then delete the NEW sensor
		void CWebServer::Cmd_DoTransferDevice(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string sidx = request::findValue(&req, "idx");
			if (sidx.empty())
				return;

			std::string newidx = request::findValue(&req, "newidx");
			if (newidx.empty())
				return;

			std::vector<std::vector<std::string>> result;

			root["status"] = "OK";
			root["title"] = "DoTransferDevice";

			result = m_sql.safe_query("SELECT HardwareID, DeviceID, Unit, Type, SubType FROM DeviceStatus WHERE (ID == '%q')", newidx.c_str());
			if (result.empty())
				return;

			int newHardwareID = std::stoi(result[0].at(0));
			std::string newDeviceID = result[0].at(1);
			int newUnit = std::stoi(result[0].at(2));
			int devType = std::stoi(result[0].at(3));
			int subType = std::stoi(result[0].at(4));

			//get last update date from old device
			result = m_sql.safe_query("SELECT LastUpdate FROM DeviceStatus WHERE (ID == '%q')", sidx.c_str());
			if (result.empty())
				return;
			std::string szLastOldDate = result[0][0];

			m_sql.safe_query("UPDATE DeviceStatus SET HardwareID = %d, DeviceID = '%q', Unit = %d, Type = %d, SubType = %d WHERE ID == '%q'", newHardwareID, newDeviceID.c_str(), newUnit, devType, subType, sidx.c_str());

			//new device could already have some logging, so let's keep this data
			//Rain
			m_sql.safe_query("UPDATE Rain SET DeviceRowID='%q' WHERE (DeviceRowID == '%q') AND (Date>'%q')", sidx.c_str(), newidx.c_str(), szLastOldDate.c_str());
			m_sql.safe_query("UPDATE Rain_Calendar SET DeviceRowID='%q' WHERE (DeviceRowID == '%q') AND (Date>'%q')", sidx.c_str(), newidx.c_str(), szLastOldDate.c_str());

			//Temperature
			m_sql.safe_query("UPDATE Temperature SET DeviceRowID='%q' WHERE (DeviceRowID == '%q') AND (Date>'%q')", sidx.c_str(), newidx.c_str(), szLastOldDate.c_str());
			m_sql.safe_query("UPDATE Temperature_Calendar SET DeviceRowID='%q' WHERE (DeviceRowID == '%q') AND (Date>'%q')", sidx.c_str(), newidx.c_str(), szLastOldDate.c_str());

			//UV
			m_sql.safe_query("UPDATE UV SET DeviceRowID='%q' WHERE (DeviceRowID == '%q') AND (Date>'%q')", sidx.c_str(), newidx.c_str(), szLastOldDate.c_str());
			m_sql.safe_query("UPDATE UV_Calendar SET DeviceRowID='%q' WHERE (DeviceRowID == '%q') AND (Date>'%q')", sidx.c_str(), newidx.c_str(), szLastOldDate.c_str());

			//Wind
			m_sql.safe_query("UPDATE Wind SET DeviceRowID='%q' WHERE (DeviceRowID == '%q') AND (Date>'%q')", sidx.c_str(), newidx.c_str(), szLastOldDate.c_str());
			m_sql.safe_query("UPDATE Wind_Calendar SET DeviceRowID='%q' WHERE (DeviceRowID == '%q') AND (Date>'%q')", sidx.c_str(), newidx.c_str(), szLastOldDate.c_str());

			//Meter
			m_sql.safe_query("UPDATE Meter SET DeviceRowID='%q' WHERE (DeviceRowID == '%q') AND (Date>'%q')", sidx.c_str(), newidx.c_str(), szLastOldDate.c_str());
			m_sql.safe_query("UPDATE Meter_Calendar SET DeviceRowID='%q' WHERE (DeviceRowID == '%q') AND (Date>'%q')", sidx.c_str(), newidx.c_str(), szLastOldDate.c_str());

			//Multimeter
			m_sql.safe_query("UPDATE MultiMeter SET DeviceRowID='%q' WHERE (DeviceRowID == '%q') AND (Date>'%q')", sidx.c_str(), newidx.c_str(), szLastOldDate.c_str());
			m_sql.safe_query("UPDATE MultiMeter_Calendar SET DeviceRowID='%q' WHERE (DeviceRowID == '%q') AND (Date>'%q')", sidx.c_str(), newidx.c_str(), szLastOldDate.c_str());

			//Fan
			m_sql.safe_query("UPDATE Fan SET DeviceRowID='%q' WHERE (DeviceRowID == '%q') AND (Date>'%q')", sidx.c_str(), newidx.c_str(), szLastOldDate.c_str());
			m_sql.safe_query("UPDATE Fan_Calendar SET DeviceRowID='%q' WHERE (DeviceRowID == '%q') AND (Date>'%q')", sidx.c_str(), newidx.c_str(), szLastOldDate.c_str());

			//Percentage
			m_sql.safe_query("UPDATE Percentage SET DeviceRowID='%q' WHERE (DeviceRowID == '%q') AND (Date>'%q')", sidx.c_str(), newidx.c_str(), szLastOldDate.c_str());
			m_sql.safe_query("UPDATE Percentage_Calendar SET DeviceRowID='%q' WHERE (DeviceRowID == '%q') AND (Date>'%q')", sidx.c_str(), newidx.c_str(), szLastOldDate.c_str());

			m_sql.DeleteDevices(newidx);

			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_GetNotifications(WebEmSession& session, const request& req, Json::Value& root)
		{
			root["status"] = "OK";
			root["title"] = "getnotifications";

			int ii = 0;

			// Add known notification systems
			for (const auto& notifier : m_notifications.m_notifiers)
			{
				root["notifiers"][ii]["name"] = notifier.first;
				root["notifiers"][ii]["description"] = notifier.first;
				ii++;
			}

			uint64_t idx = 0;
			if (!request::findValue(&req, "idx").empty())
			{
				idx = std::stoull(request::findValue(&req, "idx"));
			}
			std::vector<_tNotification> notifications = m_notifications.GetNotifications(idx);
			if (!notifications.empty())
			{
				ii = 0;
				for (const auto& n : notifications)
				{
					root["result"][ii]["idx"] = Json::Value::UInt64(n.ID);
					std::string sParams = n.Params;
					if (sParams.empty())
					{
						sParams = "S";
					}
					root["result"][ii]["Params"] = sParams;
					root["result"][ii]["Priority"] = n.Priority;
					root["result"][ii]["SendAlways"] = n.SendAlways;
					root["result"][ii]["CustomMessage"] = n.CustomMessage;
					root["result"][ii]["CustomAction"] = CURLEncode::URLEncode(n.CustomAction);
					root["result"][ii]["ActiveSystems"] = n.ActiveSystems;
					ii++;
				}
			}
		}

		void CWebServer::Cmd_GetSharedUserDevices(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["title"] = "GetSharedUserDevices";

			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT DeviceRowID FROM SharedDevices WHERE (SharedUserID == '%q')", idx.c_str());
			if (!result.empty())
			{
				int ii = 0;
				for (const auto& sd : result)
				{
					root["result"][ii]["DeviceRowIdx"] = sd[0];
					ii++;
				}
			}
			root["status"] = "OK";
		}

		void CWebServer::Cmd_SetSharedUserDevices(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			std::string idx = request::findValue(&req, "idx");
			std::string userdevices = CURLEncode::URLDecode(request::findValue(&req, "devices"));
			if (idx.empty())
				return;
			root["title"] = "SetSharedUserDevices";
			std::vector<std::string> strarray;
			StringSplit(userdevices, ";", strarray);

			// First make a backup of the favorite devices before deleting the devices for this user, then add the (new) onces and restore favorites
			m_sql.safe_query("UPDATE SharedDevices SET SharedUserID = 0 WHERE SharedUserID == '%q' and Favorite == 1", idx.c_str());
			m_sql.safe_query("DELETE FROM SharedDevices WHERE SharedUserID == '%q'", idx.c_str());

			int nDevices = static_cast<int>(strarray.size());
			for (int ii = 0; ii < nDevices; ii++)
			{
				m_sql.safe_query("INSERT INTO SharedDevices (SharedUserID,DeviceRowID) VALUES ('%q','%q')", idx.c_str(), strarray[ii].c_str());
				m_sql.safe_query("UPDATE SharedDevices SET Favorite = 1 WHERE SharedUserid == '%q' AND DeviceRowID IN (SELECT DeviceRowID FROM SharedDevices WHERE SharedUserID == 0)",
					idx.c_str());
			}
			m_sql.safe_query("DELETE FROM SharedDevices WHERE SharedUserID == 0");
			LoadUsers();
			root["status"] = "OK";
		}

		void CWebServer::Cmd_ClearUserDevices(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "ClearUserDevices";
			m_sql.safe_query("DELETE FROM SharedDevices WHERE SharedUserID == '%q'", idx.c_str());
			LoadUsers();
		}

		void CWebServer::Cmd_SetUsed(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			std::string sused = request::findValue(&req, "used");
			if ((idx.empty()) || (sused.empty()))
				return;
			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT Type,SubType,HardwareID,CustomImage FROM DeviceStatus WHERE (ID == '%q')", idx.c_str());
			if (result.empty())
				return;

			std::string deviceid = request::findValue(&req, "deviceid");
			std::string name = HTMLSanitizer::Sanitize(request::findValue(&req, "name"));
			std::string description = HTMLSanitizer::Sanitize(request::findValue(&req, "description"));
			std::string sswitchtype = request::findValue(&req, "switchtype");
			std::string maindeviceidx = request::findValue(&req, "maindeviceidx");
			std::string addjvalue = request::findValue(&req, "addjvalue");
			std::string addjmulti = request::findValue(&req, "addjmulti");
			std::string addjvalue2 = request::findValue(&req, "addjvalue2");
			std::string addjmulti2 = request::findValue(&req, "addjmulti2");
			std::string setPoint = request::findValue(&req, "setpoint");
			std::string state = request::findValue(&req, "state");
			std::string mode = request::findValue(&req, "mode");
			std::string until = request::findValue(&req, "until");
			std::string clock = request::findValue(&req, "clock");
			std::string tmode = request::findValue(&req, "tmode");
			std::string fmode = request::findValue(&req, "fmode");
			std::string sCustomImage = request::findValue(&req, "customimage");

			std::string strunit = request::findValue(&req, "unit");
			std::string strParam1 = HTMLSanitizer::Sanitize(base64_decode(request::findValue(&req, "strparam1")));
			std::string strParam2 = HTMLSanitizer::Sanitize(base64_decode(request::findValue(&req, "strparam2")));
			std::string tmpstr = request::findValue(&req, "protected");
			bool bHasstrParam1 = request::hasValue(&req, "strparam1");
			int iProtected = (tmpstr == "true") ? 1 : 0;

			std::string sOptions = HTMLSanitizer::Sanitize(base64_decode(request::findValue(&req, "options")));
			std::string devoptions = HTMLSanitizer::Sanitize(CURLEncode::URLDecode(request::findValue(&req, "devoptions")));
			std::string EnergyMeterMode = CURLEncode::URLDecode(request::findValue(&req, "EnergyMeterMode"));

			char szTmp[200];

			bool bHaveUser = (!session.username.empty());
			// int iUser = -1;
			if (bHaveUser)
			{
				// iUser = FindUser(session.username.c_str());
			}

			int switchtype = -1;
			if (!sswitchtype.empty())
				switchtype = atoi(sswitchtype.c_str());

			int used = (sused == "true") ? 1 : 0;
			if (!maindeviceidx.empty())
				used = 0;

			std::vector<std::string> sd = result[0];
			unsigned char dType = atoi(sd[0].c_str());
			unsigned char dSubType = atoi(sd[1].c_str());
			int HwdID = atoi(sd[2].c_str());
			std::string sHwdID = sd[2];
			int OldCustomImage = atoi(sd[3].c_str());

			int CustomImage = (!sCustomImage.empty()) ? std::stoi(sCustomImage) : OldCustomImage;

			// Strip trailing spaces in 'name'
			name = stdstring_trim(name);

			// Strip trailing spaces in 'description'
			description = stdstring_trim(description);

			if (!setPoint.empty() || !state.empty())
			{
				double tempcelcius = atof(setPoint.c_str());
				if (m_sql.m_tempunit == TEMPUNIT_F)
				{
					// Convert back to Celsius
					tempcelcius = ConvertToCelsius(tempcelcius);
				}
				sprintf(szTmp, "%.2f", tempcelcius);

				if (dType != pTypeEvohomeZone && dType != pTypeEvohomeWater) // sql update now done in setsetpoint for evohome devices
				{
					m_sql.safe_query("UPDATE DeviceStatus SET Used=%d, sValue='%q' WHERE (ID == '%q')", used, szTmp, idx.c_str());
				}
			}
			if (name.empty())
			{
				m_sql.safe_query("UPDATE DeviceStatus SET Used=%d WHERE (ID == '%q')", used, idx.c_str());
			}
			else
			{
				if (switchtype == -1)
				{
					m_sql.safe_query("UPDATE DeviceStatus SET Used=%d, Name='%q', Description='%q', CustomImage=%d WHERE (ID == '%q')", used, name.c_str(), description.c_str(),
						CustomImage, idx.c_str());
				}
				else
				{
					m_sql.safe_query("UPDATE DeviceStatus SET Used=%d, Name='%q', Description='%q', SwitchType=%d, CustomImage=%d WHERE (ID == '%q')", used, name.c_str(),
						description.c_str(), switchtype, CustomImage, idx.c_str());
				}
			}

			if (bHasstrParam1)
			{
				m_sql.safe_query("UPDATE DeviceStatus SET StrParam1='%q', StrParam2='%q' WHERE (ID == '%q')", strParam1.c_str(), strParam2.c_str(), idx.c_str());
			}

			m_sql.safe_query("UPDATE DeviceStatus SET Protected=%d WHERE (ID == '%q')", iProtected, idx.c_str());

			if (!setPoint.empty() || !state.empty())
			{
				int urights = 3;
				if (bHaveUser)
				{
					int iUser = FindUser(session.username.c_str());
					if (iUser != -1)
					{
						urights = static_cast<int>(m_users[iUser].userrights);
						_log.Log(LOG_STATUS, "User: %s initiated a SetPoint command", m_users[iUser].Username.c_str());
					}
				}
				if (urights < 1)
					return;
				if (dType == pTypeEvohomeWater)
					m_mainworker.SetSetPoint(idx, (state == "On") ? 1.0F : 0.0F, mode, until); // FIXME float not guaranteed precise?
				else if (dType == pTypeEvohomeZone)
					m_mainworker.SetSetPoint(idx, static_cast<float>(atof(setPoint.c_str())), mode, until);
				else
					m_mainworker.SetSetPoint(idx, static_cast<float>(atof(setPoint.c_str())));
			}

			if (!strunit.empty())
			{
				bool bUpdateUnit = true;
#ifdef ENABLE_PYTHON
				// check if HW is plugin
				std::vector<std::vector<std::string>> result;
				result = m_sql.safe_query("SELECT Type FROM Hardware WHERE (ID == %d)", HwdID);
				if (!result.empty())
				{
					_eHardwareTypes Type = (_eHardwareTypes)std::stoi(result[0][0]);
					if (Type == HTYPE_PythonPlugin)
					{
						bUpdateUnit = false;
						_log.Log(LOG_ERROR, "CWebServer::Cmd_SetUsed: Not allowed to change unit of device owned by plugin %u!", HwdID);
					}
				}
#endif
				if (bUpdateUnit)
				{
					m_sql.safe_query("UPDATE DeviceStatus SET Unit='%q' WHERE (ID == '%q')", strunit.c_str(), idx.c_str());
				}
			}
			// FIXME evohome ...we need the zone id to update the correct zone...but this should be ok as a generic call?
			if (!deviceid.empty())
			{
				m_sql.safe_query("UPDATE DeviceStatus SET DeviceID='%q' WHERE (ID == '%q')", deviceid.c_str(), idx.c_str());
			}
			if (!addjvalue.empty())
			{
				double faddjvalue = atof(addjvalue.c_str());
				m_sql.safe_query("UPDATE DeviceStatus SET AddjValue=%f WHERE (ID == '%q')", faddjvalue, idx.c_str());
			}
			if (!addjmulti.empty())
			{
				double faddjmulti = atof(addjmulti.c_str());
				if (faddjmulti == 0)
					faddjmulti = 1;
				m_sql.safe_query("UPDATE DeviceStatus SET AddjMulti=%f WHERE (ID == '%q')", faddjmulti, idx.c_str());
			}
			if (!addjvalue2.empty())
			{
				double faddjvalue2 = atof(addjvalue2.c_str());
				m_sql.safe_query("UPDATE DeviceStatus SET AddjValue2=%f WHERE (ID == '%q')", faddjvalue2, idx.c_str());
			}
			if (!addjmulti2.empty())
			{
				double faddjmulti2 = atof(addjmulti2.c_str());
				if (faddjmulti2 == 0)
					faddjmulti2 = 1;
				m_sql.safe_query("UPDATE DeviceStatus SET AddjMulti2=%f WHERE (ID == '%q')", faddjmulti2, idx.c_str());
			}
			if (!EnergyMeterMode.empty())
			{
				auto options = m_sql.GetDeviceOptions(idx);
				options["EnergyMeterMode"] = EnergyMeterMode;
				uint64_t ullidx = std::stoull(idx);
				m_sql.SetDeviceOptions(ullidx, options);
			}

			if (!devoptions.empty())
			{
				m_sql.safe_query("UPDATE DeviceStatus SET Options='%q' WHERE (ID == '%q')", devoptions.c_str(), idx.c_str());
			}

			if (used == 0)
			{
				bool bRemoveSubDevices = (request::findValue(&req, "RemoveSubDevices") == "true");

				if (bRemoveSubDevices)
				{
					// if this device was a slave device, remove it
					m_sql.safe_query("DELETE FROM LightSubDevices WHERE (DeviceRowID == '%q')", idx.c_str());
				}
				m_sql.safe_query("DELETE FROM LightSubDevices WHERE (ParentID == '%q')", idx.c_str());

				m_sql.safe_query("DELETE FROM Timers WHERE (DeviceRowID == '%q')", idx.c_str());
			}

			// Save device options
			if (!sOptions.empty())
			{
				uint64_t ullidx = std::stoull(idx);
				m_sql.SetDeviceOptions(ullidx, m_sql.BuildDeviceOptions(sOptions, false));
			}

			if (!maindeviceidx.empty())
			{
				if (maindeviceidx != idx)
				{
					// this is a sub device for another light/switch
					// first check if it is not already a sub device
					result = m_sql.safe_query("SELECT ID FROM LightSubDevices WHERE (DeviceRowID=='%q') AND (ParentID =='%q')", idx.c_str(), maindeviceidx.c_str());
					if (result.empty())
					{
						// no it is not, add it
						m_sql.safe_query("INSERT INTO LightSubDevices (DeviceRowID, ParentID) VALUES ('%q','%q')", idx.c_str(), maindeviceidx.c_str());
					}
				}
			}
			if ((used == 0) && (maindeviceidx.empty()))
			{
				// really remove it, including log etc
				m_sql.DeleteDevices(idx);
			}
			else
			{
#ifdef ENABLE_PYTHON
				// Notify plugin framework about the change
				m_mainworker.m_pluginsystem.DeviceModified(atoi(idx.c_str()));
#endif
			}
			if (!result.empty())
			{
				root["status"] = "OK";
				root["title"] = "SetUsed";
			}
			if (m_sql.m_bEnableEventSystem)
				m_mainworker.m_eventsystem.GetCurrentStates();
		}

		void CWebServer::Cmd_GetSettings(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::vector<std::vector<std::string>> result;
			char szTmp[100];

			result = m_sql.safe_query("SELECT Key, nValue, sValue FROM Preferences");
			if (result.empty())
				return;
			root["status"] = "OK";
			root["title"] = "settings";
			root["cloudenabled"] = false;

			for (const auto& sd : result)
			{
				std::string Key = sd[0];
				int nValue = atoi(sd[1].c_str());
				std::string sValue = sd[2];

				if (Key == "Location")
				{
					std::vector<std::string> strarray;
					StringSplit(sValue, ";", strarray);

					if (strarray.size() == 2)
					{
						root["Location"]["Latitude"] = strarray[0];
						root["Location"]["Longitude"] = strarray[1];
					}
				}
				/* RK: notification settings */
				if (m_notifications.IsInConfig(Key))
				{
					if (sValue.empty() && nValue > 0)
					{
						root[Key] = nValue;
					}
					else
					{
						root[Key] = sValue;
					}
				}
				else if (Key == "DashboardType")
				{
					root["DashboardType"] = nValue;
				}
				else if (Key == "MobileType")
				{
					root["MobileType"] = nValue;
				}
				else if (Key == "LightHistoryDays")
				{
					root["LightHistoryDays"] = nValue;
				}
				else if (Key == "5MinuteHistoryDays")
				{
					root["ShortLogDays"] = nValue;
				}
				else if (Key == "ShortLogAddOnlyNewValues")
				{
					root["ShortLogAddOnlyNewValues"] = nValue;
				}
				else if (Key == "ShortLogInterval")
				{
					root["ShortLogInterval"] = nValue;
				}
				else if (Key == "SecPassword")
				{
					root["SecPassword"] = sValue;
				}
				else if (Key == "ProtectionPassword")
				{
					root["ProtectionPassword"] = sValue;
				}
				else if (Key == "WebLocalNetworks")
				{
					root["WebLocalNetworks"] = sValue;
				}
				else if (Key == "RandomTimerFrame")
				{
					root["RandomTimerFrame"] = nValue;
				}
				else if (Key == "MeterDividerEnergy")
				{
					root["EnergyDivider"] = nValue;
				}
				else if (Key == "MeterDividerGas")
				{
					root["GasDivider"] = nValue;
				}
				else if (Key == "MeterDividerWater")
				{
					root["WaterDivider"] = nValue;
				}
				else if (Key == "ElectricVoltage")
				{
					root["ElectricVoltage"] = nValue;
				}
				else if (Key == "MaxElectricPower")
				{
					root["MaxElectricPower"] = nValue;
				}
				else if (Key == "CM113DisplayType")
				{
					root["CM113DisplayType"] = nValue;
				}
				else if (Key == "UseAutoUpdate")
				{
					root["UseAutoUpdate"] = nValue;
				}
				else if (Key == "UseAutoBackup")
				{
					root["UseAutoBackup"] = nValue;
				}
				else if (Key == "Rego6XXType")
				{
					root["Rego6XXType"] = nValue;
				}
				else if (Key == "CostEnergy")
				{
					sprintf(szTmp, "%.4f", (float)(nValue) / 10000.0F);
					root["CostEnergy"] = szTmp;
				}
				else if (Key == "CostEnergyT2")
				{
					sprintf(szTmp, "%.4f", (float)(nValue) / 10000.0F);
					root["CostEnergyT2"] = szTmp;
				}
				else if (Key == "CostEnergyR1")
				{
					sprintf(szTmp, "%.4f", (float)(nValue) / 10000.0F);
					root["CostEnergyR1"] = szTmp;
				}
				else if (Key == "CostEnergyR2")
				{
					sprintf(szTmp, "%.4f", (float)(nValue) / 10000.0F);
					root["CostEnergyR2"] = szTmp;
				}
				else if (Key == "CostGas")
				{
					sprintf(szTmp, "%.4f", (float)(nValue) / 10000.0F);
					root["CostGas"] = szTmp;
				}
				else if (Key == "CostWater")
				{
					sprintf(szTmp, "%.4f", (float)(nValue) / 10000.0F);
					root["CostWater"] = szTmp;
				}
				else if (Key == "ActiveTimerPlan")
				{
					root["ActiveTimerPlan"] = nValue;
				}
				else if (Key == "DoorbellCommand")
				{
					root["DoorbellCommand"] = nValue;
				}
				else if (Key == "SmartMeterType")
				{
					root["SmartMeterType"] = nValue;
				}
				else if (Key == "EnableTabFloorplans")
				{
					root["EnableTabFloorplans"] = nValue;
				}
				else if (Key == "EnableTabLights")
				{
					root["EnableTabLights"] = nValue;
				}
				else if (Key == "EnableTabTemp")
				{
					root["EnableTabTemp"] = nValue;
				}
				else if (Key == "EnableTabWeather")
				{
					root["EnableTabWeather"] = nValue;
				}
				else if (Key == "EnableTabUtility")
				{
					root["EnableTabUtility"] = nValue;
				}
				else if (Key == "EnableTabScenes")
				{
					root["EnableTabScenes"] = nValue;
				}
				else if (Key == "EnableTabCustom")
				{
					root["EnableTabCustom"] = nValue;
				}
				else if (Key == "NotificationSensorInterval")
				{
					root["NotificationSensorInterval"] = nValue;
				}
				else if (Key == "NotificationSwitchInterval")
				{
					root["NotificationSwitchInterval"] = nValue;
				}
				else if (Key == "RemoteSharedPort")
				{
					root["RemoteSharedPort"] = nValue;
				}
				else if (Key == "Language")
				{
					root["Language"] = sValue;
				}
				else if (Key == "Title")
				{
					root["Title"] = sValue;
				}
				else if (Key == "WindUnit")
				{
					root["WindUnit"] = nValue;
				}
				else if (Key == "TempUnit")
				{
					root["TempUnit"] = nValue;
				}
				else if (Key == "WeightUnit")
				{
					root["WeightUnit"] = nValue;
				}
				else if (Key == "AllowPlainBasicAuth")
				{
					root["AllowPlainBasicAuth"] = nValue;
				}
				else if (Key == "ReleaseChannel")
				{
					root["ReleaseChannel"] = nValue;
				}
				else if (Key == "RaspCamParams")
				{
					root["RaspCamParams"] = sValue;
				}
				else if (Key == "UVCParams")
				{
					root["UVCParams"] = sValue;
				}
				else if (Key == "AcceptNewHardware")
				{
					root["AcceptNewHardware"] = nValue;
				}
				else if (Key == "HideDisabledHardwareSensors")
				{
					root["HideDisabledHardwareSensors"] = nValue;
				}
				else if (Key == "ShowUpdateEffect")
				{
					root["ShowUpdateEffect"] = nValue;
				}
				else if (Key == "DegreeDaysBaseTemperature")
				{
					root["DegreeDaysBaseTemperature"] = sValue;
				}
				else if (Key == "EnableEventScriptSystem")
				{
					root["EnableEventScriptSystem"] = nValue;
				}
				else if (Key == "EventSystemLogFullURL")
				{
					root["EventSystemLogFullURL"] = nValue;
				}
				else if (Key == "DisableDzVentsSystem")
				{
					root["DisableDzVentsSystem"] = nValue;
				}
				else if (Key == "DzVentsLogLevel")
				{
					root["DzVentsLogLevel"] = nValue;
				}
				else if (Key == "LogEventScriptTrigger")
				{
					root["LogEventScriptTrigger"] = nValue;
				}
				else if (Key == "(1WireSensorPollPeriod")
				{
					root["1WireSensorPollPeriod"] = nValue;
				}
				else if (Key == "(1WireSwitchPollPeriod")
				{
					root["1WireSwitchPollPeriod"] = nValue;
				}
				else if (Key == "SecOnDelay")
				{
					root["SecOnDelay"] = nValue;
				}
				else if (Key == "AllowWidgetOrdering")
				{
					root["AllowWidgetOrdering"] = nValue;
				}
				else if (Key == "FloorplanPopupDelay")
				{
					root["FloorplanPopupDelay"] = nValue;
				}
				else if (Key == "FloorplanFullscreenMode")
				{
					root["FloorplanFullscreenMode"] = nValue;
				}
				else if (Key == "FloorplanAnimateZoom")
				{
					root["FloorplanAnimateZoom"] = nValue;
				}
				else if (Key == "FloorplanShowSensorValues")
				{
					root["FloorplanShowSensorValues"] = nValue;
				}
				else if (Key == "FloorplanShowSwitchValues")
				{
					root["FloorplanShowSwitchValues"] = nValue;
				}
				else if (Key == "FloorplanShowSceneNames")
				{
					root["FloorplanShowSceneNames"] = nValue;
				}
				else if (Key == "FloorplanRoomColour")
				{
					root["FloorplanRoomColour"] = sValue;
				}
				else if (Key == "FloorplanActiveOpacity")
				{
					root["FloorplanActiveOpacity"] = nValue;
				}
				else if (Key == "FloorplanInactiveOpacity")
				{
					root["FloorplanInactiveOpacity"] = nValue;
				}
				else if (Key == "SensorTimeout")
				{
					root["SensorTimeout"] = nValue;
				}
				else if (Key == "BatteryLowNotification")
				{
					root["BatterLowLevel"] = nValue;
				}
				else if (Key == "WebTheme")
				{
					root["WebTheme"] = sValue;
				}
				else if (Key == "MyDomoticzSubsystems")
				{
					root["MyDomoticzSubsystems"] = nValue;
				}
				else if (Key == "SendErrorsAsNotification")
				{
					root["SendErrorsAsNotification"] = nValue;
				}
				else if (Key == "DeltaTemperatureLog")
				{
					root[Key] = sValue;
				}
				else if (Key == "IFTTTEnabled")
				{
					root["IFTTTEnabled"] = nValue;
				}
				else if (Key == "IFTTTAPI")
				{
					root["IFTTTAPI"] = sValue;
				}
			}
		}

		void CWebServer::Cmd_GetLightLog(WebEmSession& session, const request& req, Json::Value& root)
		{
			uint64_t idx = 0;
			if (!request::findValue(&req, "idx").empty())
			{
				idx = std::stoull(request::findValue(&req, "idx"));
			}
			std::vector<std::vector<std::string>> result;
			// First get Device Type/SubType
			result = m_sql.safe_query("SELECT Type, SubType, SwitchType, Options FROM DeviceStatus WHERE (ID == %" PRIu64 ")", idx);
			if (result.empty())
				return;

			unsigned char dType = atoi(result[0][0].c_str());
			unsigned char dSubType = atoi(result[0][1].c_str());
			_eSwitchType switchtype = (_eSwitchType)atoi(result[0][2].c_str());
			std::map<std::string, std::string> options = m_sql.BuildDeviceOptions(result[0][3]);

			if ((dType != pTypeLighting1) && (dType != pTypeLighting2) && (dType != pTypeLighting3) && (dType != pTypeLighting4) && (dType != pTypeLighting5) &&
				(dType != pTypeLighting6) && (dType != pTypeFan) && (dType != pTypeColorSwitch) && (dType != pTypeSecurity1) && (dType != pTypeSecurity2) && (dType != pTypeEvohome) &&
				(dType != pTypeEvohomeRelay) && (dType != pTypeCurtain) && (dType != pTypeBlinds) && (dType != pTypeRFY) && (dType != pTypeRego6XXValue) && (dType != pTypeChime) &&
				(dType != pTypeThermostat2) && (dType != pTypeThermostat3) && (dType != pTypeThermostat4) && (dType != pTypeRemote) && (dType != pTypeGeneralSwitch) &&
				(dType != pTypeHomeConfort) && (dType != pTypeFS20) && (!((dType == pTypeRadiator1) && (dSubType == sTypeSmartwaresSwitchRadiator))) && (dType != pTypeHunter))
				return; // no light device! we should not be here!

			root["status"] = "OK";
			root["title"] = "getlightlog";

			result = m_sql.safe_query("SELECT ROWID, nValue, sValue, User, Date FROM LightingLog WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date DESC", idx);
			if (!result.empty())
			{
				std::map<std::string, std::string> selectorStatuses;
				if (switchtype == STYPE_Selector)
				{
					GetSelectorSwitchStatuses(options, selectorStatuses);
				}

				int ii = 0;
				for (const auto& sd : result)
				{
					std::string lidx = sd.at(0);
					int nValue = atoi(sd.at(1).c_str());
					std::string sValue = sd.at(2);
					std::string sUser = sd.at(3);
					std::string ldate = sd.at(4);

					// add light details
					std::string lstatus;
					std::string ldata;
					int llevel = 0;
					bool bHaveDimmer = false;
					bool bHaveSelector = false;
					bool bHaveGroupCmd = false;
					int maxDimLevel = 0;

					if (switchtype == STYPE_Media)
					{
						if (sValue == "0")
							continue; // skip 0-values in log for MediaPlayers
						lstatus = sValue;
						ldata = lstatus;
					}
					else if (switchtype == STYPE_Selector)
					{
						if (ii == 0)
						{
							bHaveSelector = true;
							maxDimLevel = (int)selectorStatuses.size();
						}
						if (!selectorStatuses.empty())
						{

							std::string sLevel = selectorStatuses[sValue];
							ldata = sLevel;
							lstatus = "Set Level: " + sLevel;
							llevel = atoi(sValue.c_str());
						}
					}
					else
					{
						GetLightStatus(dType, dSubType, switchtype, nValue, sValue, lstatus, llevel, bHaveDimmer, maxDimLevel, bHaveGroupCmd);
						ldata = lstatus;
					}

					if (ii == 0)
					{
						// Log these parameters once
						root["HaveDimmer"] = bHaveDimmer;
						root["result"][ii]["MaxDimLevel"] = maxDimLevel;
						root["HaveGroupCmd"] = bHaveGroupCmd;
						root["HaveSelector"] = bHaveSelector;
					}

					// Corrent names for certain switch types
					switch (switchtype)
					{
					case STYPE_Contact:
						ldata = (ldata == "On") ? "Open" : "Closed";
						break;
					case STYPE_DoorContact:
						ldata = (ldata == "On") ? "Open" : "Closed";
						break;
					case STYPE_DoorLock:
						ldata = (ldata == "On") ? "Locked" : "Unlocked";
						break;
					case STYPE_DoorLockInverted:
						ldata = (ldata == "On") ? "Unlocked" : "Locked";
						break;
					}

					root["result"][ii]["idx"] = lidx;
					root["result"][ii]["Date"] = ldate;
					root["result"][ii]["Data"] = ldata;
					root["result"][ii]["Status"] = lstatus;
					root["result"][ii]["Level"] = llevel;
					root["result"][ii]["User"] = sUser;
					ii++;
				}
			}
		}

		void CWebServer::Cmd_GetTextLog(WebEmSession& session, const request& req, Json::Value& root)
		{
			uint64_t idx = 0;
			if (!request::findValue(&req, "idx").empty())
			{
				idx = std::stoull(request::findValue(&req, "idx"));
			}
			std::vector<std::vector<std::string>> result;

			root["status"] = "OK";
			root["title"] = "gettextlog";

			result = m_sql.safe_query("SELECT ROWID, sValue, User, Date FROM LightingLog WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date DESC", idx);
			if (!result.empty())
			{
				int ii = 0;
				for (const auto& sd : result)
				{
					root["result"][ii]["idx"] = sd[0];
					root["result"][ii]["Data"] = sd[1];
					root["result"][ii]["User"] = sd[2];
					root["result"][ii]["Date"] = sd[3];
					ii++;
				}
			}
		}

		void CWebServer::Cmd_GetSceneLog(WebEmSession& session, const request& req, Json::Value& root)
		{
			uint64_t idx = 0;
			if (!request::findValue(&req, "idx").empty())
			{
				idx = std::stoull(request::findValue(&req, "idx"));
			}
			std::vector<std::vector<std::string>> result;

			root["status"] = "OK";
			root["title"] = "getscenelog";

			result = m_sql.safe_query("SELECT ROWID, nValue, User, Date FROM SceneLog WHERE (SceneRowID==%" PRIu64 ") ORDER BY Date DESC", idx);
			if (!result.empty())
			{
				int ii = 0;
				for (const auto& sd : result)
				{
					root["result"][ii]["idx"] = sd[0];
					int nValue = atoi(sd[1].c_str());
					root["result"][ii]["Data"] = (nValue == 0) ? "Off" : "On";
					root["result"][ii]["User"] = sd[2];
					root["result"][ii]["Date"] = sd[3];
					ii++;
				}
			}
		}

		extern std::map<std::string, http::server::connection::_tRemoteClients> m_remote_web_clients;

		void CWebServer::Cmd_RemoteWebClientsLog(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			int ii = 0;
			root["title"] = "rclientslog";
			for (const auto& itt_rc : m_remote_web_clients)
			{
				char timestring[128];
				timestring[0] = 0;
				struct tm timeinfo;
				localtime_r(&itt_rc.second.last_seen, &timeinfo);

				strftime(timestring, sizeof(timestring), "%a, %d %b %Y %H:%M:%S %z", &timeinfo);

				root["result"][ii]["date"] = timestring;
				root["result"][ii]["address"] = itt_rc.second.host_remote_endpoint_address_;
				root["result"][ii]["port"] = itt_rc.second.host_local_endpoint_port_;
				root["result"][ii]["req"] = itt_rc.second.host_last_request_uri_;
				ii++;
			}
			root["status"] = "OK";
		}

		void CWebServer::Cmd_HandleGraph(WebEmSession& session, const request& req, Json::Value& root)
		{
			uint64_t idx = 0;
			if (!request::findValue(&req, "idx").empty())
			{
				idx = std::stoull(request::findValue(&req, "idx"));
			}

			std::vector<std::vector<std::string>> result;
			char szTmp[300];

			std::string sensor = request::findValue(&req, "sensor");
			if (sensor.empty())
				return;
			std::string sensorarea = request::findValue(&req, "sensorarea");
			std::string srange = request::findValue(&req, "range");
			std::string sgroupby = request::findValue(&req, "groupby");
			if (srange.empty() && sgroupby.empty())
				return;

			time_t now = mytime(nullptr);
			struct tm tm1;
			localtime_r(&now, &tm1);

			result = m_sql.safe_query("SELECT Type, SubType, SwitchType, AddjValue, AddjMulti, AddjValue2, Options FROM DeviceStatus WHERE (ID == %" PRIu64 ")", idx);
			if (result.empty())
				return;

			unsigned char dType = atoi(result[0][0].c_str());
			unsigned char dSubType = atoi(result[0][1].c_str());
			_eMeterType metertype = (_eMeterType)atoi(result[0][2].c_str());
			_log.Debug(DEBUG_WEBSERVER, "CWebServer::Cmd_HandleGraph() : dType:%02X  dSubType:%02X  metertype:%d", dType, dSubType, int(metertype));
			if ((dType == pTypeP1Power) || (dType == pTypeENERGY) || (dType == pTypePOWER) || (dType == pTypeCURRENTENERGY) || ((dType == pTypeGeneral) && (dSubType == sTypeKwh)))
			{
				metertype = MTYPE_ENERGY;
			}
			else if (dType == pTypeP1Gas)
				metertype = MTYPE_GAS;
			else if ((dType == pTypeRego6XXValue) && (dSubType == sTypeRego6XXCounter))
				metertype = MTYPE_COUNTER;

			// Special case of managed counter: Usage instead of Value in Meter table, and we don't want to calculate last value
			bool bIsManagedCounter = (dType == pTypeGeneral) && (dSubType == sTypeManagedCounter);

			double AddjValue = atof(result[0][3].c_str());
			double AddjMulti = atof(result[0][4].c_str());
			double AddjValue2 = atof(result[0][5].c_str());
			std::string sOptions = result[0][6];
			std::map<std::string, std::string> options = m_sql.BuildDeviceOptions(sOptions);

			double divider = m_sql.GetCounterDivider(int(metertype), int(dType), float(AddjValue2));

			double meteroffset = AddjValue;

			std::string dbasetable;
			if (srange == "day")
			{
				if (sensor == "temp")
					dbasetable = "Temperature";
				else if (sensor == "rain")
					dbasetable = "Rain";
				else if (sensor == "Percentage")
					dbasetable = "Percentage";
				else if (sensor == "fan")
					dbasetable = "Fan";
				else if (sensor == "counter")
				{
					Cmd_GetCosts(session, req, root);

					if ((dType == pTypeP1Power) || (dType == pTypeCURRENT) || (dType == pTypeCURRENTENERGY))
					{
						dbasetable = "MultiMeter";
					}
					else
					{
						dbasetable = "Meter";
					}
				}
				else if ((sensor == "wind") || (sensor == "winddir"))
					dbasetable = "Wind";
				else if (sensor == "uv")
					dbasetable = "UV";
				else
					return;
			}
			else
			{
				// week,year,month
				if (sensor == "temp")
					dbasetable = "Temperature_Calendar";
				else if (sensor == "rain")
					dbasetable = "Rain_Calendar";
				else if (sensor == "Percentage")
					dbasetable = "Percentage_Calendar";
				else if (sensor == "fan")
					dbasetable = "Fan_Calendar";
				else if (sensor == "counter")
				{
					Cmd_GetCosts(session, req, root);

					if (dType == pTypeP1Power
						|| dType == pTypeCURRENT
						|| dType == pTypeCURRENTENERGY
						|| dType == pTypeAirQuality
						|| dType == pTypeLux
						|| dType == pTypeWEIGHT
						|| dType == pTypeUsage
						|| dType == pTypeGeneral && dSubType == sTypeVisibility
						|| dType == pTypeGeneral && dSubType == sTypeDistance
						|| dType == pTypeGeneral && dSubType == sTypeSolarRadiation
						|| dType == pTypeGeneral && dSubType == sTypeSoilMoisture
						|| dType == pTypeGeneral && dSubType == sTypeLeafWetness
						|| dType == pTypeGeneral && dSubType == sTypeVoltage
						|| dType == pTypeGeneral && dSubType == sTypeCurrent
						|| dType == pTypeGeneral && dSubType == sTypePressure
						|| dType == pTypeGeneral && dSubType == sTypeSoundLevel
						|| dType == pTypeRFXSensor && dSubType == sTypeRFXSensorAD
						|| dType == pTypeRFXSensor && dSubType == sTypeRFXSensorVolt
						) {
						dbasetable = "MultiMeter_Calendar";
					}
					else {
						dbasetable = "Meter_Calendar";
					}
				}
				else if ((sensor == "wind") || (sensor == "winddir"))
					dbasetable = "Wind_Calendar";
				else if (sensor == "uv")
					dbasetable = "UV_Calendar";
				else
					return;
			}
			unsigned char tempsign = m_sql.m_tempsign[0];
			int iPrev;

			if (srange == "day")
			{
				if (sensor == "temp")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					result = m_sql.safe_query("SELECT Temperature, Chill, Humidity, Barometer, Date, SetPoint FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC",
						dbasetable.c_str(), idx);
					if (!result.empty())
					{
						int ii = 0;
						for (const auto& sd : result)
						{
							root["result"][ii]["d"] = sd[4].substr(0, 16);
							if (dType == pTypeRego6XXTemp
								|| dType == pTypeTEMP
								|| dType == pTypeTEMP_HUM
								|| dType == pTypeTEMP_HUM_BARO
								|| dType == pTypeTEMP_BARO
								|| dType == pTypeWIND && dSubType == sTypeWIND4
								|| dType == pTypeUV && dSubType == sTypeUV3
								|| dType == pTypeThermostat1
								|| dType == pTypeRadiator1
								|| dType == pTypeRFXSensor && dSubType == sTypeRFXSensorTemp
								|| dType == pTypeGeneral && dSubType == sTypeSystemTemp
								|| dType == pTypeGeneral && dSubType == sTypeBaro
								|| dType == pTypeThermostat && dSubType == sTypeThermSetpoint
								|| dType == pTypeEvohomeZone
								|| dType == pTypeEvohomeWater
								)
							{
								double tvalue = ConvertTemperature(atof(sd[0].c_str()), tempsign);
								root["result"][ii]["te"] = tvalue;
							}
							if (((dType == pTypeWIND) && (dSubType == sTypeWIND4)) || ((dType == pTypeWIND) && (dSubType == sTypeWINDNoTemp)))
							{
								double tvalue = ConvertTemperature(atof(sd[1].c_str()), tempsign);
								root["result"][ii]["ch"] = tvalue;
							}
							if ((dType == pTypeHUM) || (dType == pTypeTEMP_HUM) || (dType == pTypeTEMP_HUM_BARO))
							{
								root["result"][ii]["hu"] = sd[2];
							}
							if ((dType == pTypeTEMP_HUM_BARO) || (dType == pTypeTEMP_BARO) || ((dType == pTypeGeneral) && (dSubType == sTypeBaro)))
							{
								if (dType == pTypeTEMP_HUM_BARO)
								{
									if (dSubType == sTypeTHBFloat)
									{
										sprintf(szTmp, "%.1f", atof(sd[3].c_str()) / 10.0F);
										root["result"][ii]["ba"] = szTmp;
									}
									else
										root["result"][ii]["ba"] = sd[3];
								}
								else if (dType == pTypeTEMP_BARO)
								{
									sprintf(szTmp, "%.1f", atof(sd[3].c_str()) / 10.0F);
									root["result"][ii]["ba"] = szTmp;
								}
								else if ((dType == pTypeGeneral) && (dSubType == sTypeBaro))
								{
									sprintf(szTmp, "%.1f", atof(sd[3].c_str()) / 10.0F);
									root["result"][ii]["ba"] = szTmp;
								}
							}
							if ((dType == pTypeEvohomeZone) || (dType == pTypeEvohomeWater))
							{
								double se = ConvertTemperature(atof(sd[5].c_str()), tempsign);
								root["result"][ii]["se"] = se;
							}

							ii++;
						}
					}
				}
				else if (sensor == "Percentage")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					result = m_sql.safe_query("SELECT Percentage, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
					if (!result.empty())
					{
						int ii = 0;
						for (const auto& sd : result)
						{
							root["result"][ii]["d"] = sd[1].substr(0, 16);
							root["result"][ii]["v"] = sd[0];
							ii++;
						}
					}
				}
				else if (sensor == "fan")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					result = m_sql.safe_query("SELECT Speed, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
					if (!result.empty())
					{
						int ii = 0;
						for (const auto& sd : result)
						{
							root["result"][ii]["d"] = sd[1].substr(0, 16);
							root["result"][ii]["v"] = sd[0];
							ii++;
						}
					}
				}
				else if (sensor == "counter")
				{
					if (dType == pTypeP1Power)
					{
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;

						result = m_sql.safe_query("SELECT Value1, Value2, Value3, Value4, Value5, Value6, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC",
							dbasetable.c_str(), idx);
						if (!result.empty())
						{
							int ii = 0;
							bool bHaveDeliverd = false;
							bool bHaveFirstValue = false;
							int64_t lastUsage1, lastUsage2, lastDeliv1, lastDeliv2;
							time_t lastTime = 0;

							int64_t firstUsage1 = 0;
							int64_t firstUsage2 = 0;
							int64_t firstDeliv1 = 0;
							int64_t firstDeliv2 = 0;

							int nMeterType = 0;
							m_sql.GetPreferencesVar("SmartMeterType", nMeterType);

							int lastDay = 0;

							for (const auto& sd : result)
							{
								if (nMeterType == 0)
								{
									int64_t actUsage1 = std::stoll(sd[0]);
									int64_t actUsage2 = std::stoll(sd[4]);
									int64_t actDeliv1 = std::stoll(sd[1]);
									int64_t actDeliv2 = std::stoll(sd[5]);
									actDeliv1 = (actDeliv1 < 10) ? 0 : actDeliv1;
									actDeliv2 = (actDeliv2 < 10) ? 0 : actDeliv2;

									std::string stime = sd[6];
									struct tm ntime;
									time_t atime;
									ParseSQLdatetime(atime, ntime, stime, -1);
									if (lastDay != ntime.tm_mday)
									{
										lastDay = ntime.tm_mday;
										firstUsage1 = actUsage1;
										firstUsage2 = actUsage2;
										firstDeliv1 = actDeliv1;
										firstDeliv2 = actDeliv2;
									}

									if (bHaveFirstValue)
									{
										if (
											(actUsage1 < lastUsage1)
											|| (actUsage2 < lastUsage2)
											|| (actDeliv1 < lastDeliv1)
											|| (actDeliv2 < lastDeliv2)
											|| (atime <= lastTime)
											)
										{
											//daylight change happened, meter changed?, ignoring  for now
											lastUsage1 = actUsage1;
											lastUsage2 = actUsage2;
											lastDeliv1 = actDeliv1;
											lastDeliv2 = actDeliv2;
											lastTime = atime;
											continue;
										}

										long curUsage1 = (long)(actUsage1 - lastUsage1);
										long curUsage2 = (long)(actUsage2 - lastUsage2);
										long curDeliv1 = (long)(actDeliv1 - lastDeliv1);
										long curDeliv2 = (long)(actDeliv2 - lastDeliv2);

										float tdiff = static_cast<float>(difftime(atime, lastTime));
										if (tdiff == 0)
											tdiff = 1;
										float tlaps = 3600.0F / tdiff;
										curUsage1 *= int(tlaps);
										curUsage2 *= int(tlaps);
										curDeliv1 *= int(tlaps);
										curDeliv2 *= int(tlaps);

										if ((curUsage1 < 0) || (curUsage1 > 100000))
											curUsage1 = 0;
										if ((curUsage2 < 0) || (curUsage2 > 100000))
											curUsage2 = 0;
										if ((curDeliv1 < 0) || (curDeliv1 > 100000))
											curDeliv1 = 0;
										if ((curDeliv2 < 0) || (curDeliv2 > 100000))
											curDeliv2 = 0;

										root["result"][ii]["d"] = sd[6].substr(0, 16);

										if ((curDeliv1 != 0) || (curDeliv2 != 0))
											bHaveDeliverd = true;

										sprintf(szTmp, "%ld", curUsage1);
										root["result"][ii]["v"] = szTmp;
										sprintf(szTmp, "%ld", curUsage2);
										root["result"][ii]["v2"] = szTmp;
										sprintf(szTmp, "%ld", curDeliv1);
										root["result"][ii]["r1"] = szTmp;
										sprintf(szTmp, "%ld", curDeliv2);
										root["result"][ii]["r2"] = szTmp;

										long pUsage1 = (long)(actUsage1 - firstUsage1);
										long pUsage2 = (long)(actUsage2 - firstUsage2);

										sprintf(szTmp, "%ld", pUsage1 + pUsage2);
										root["result"][ii]["eu"] = szTmp;
										if (bHaveDeliverd)
										{
											long pDeliv1 = (long)(actDeliv1 - firstDeliv1);
											long pDeliv2 = (long)(actDeliv2 - firstDeliv2);
											sprintf(szTmp, "%ld", pDeliv1 + pDeliv2);
											root["result"][ii]["eg"] = szTmp;
										}

										ii++;
									}
									else
									{
										bHaveFirstValue = true;
										if ((ntime.tm_hour != 0) && (ntime.tm_min != 0))
										{
											struct tm ltime;
											localtime_r(&atime, &tm1);
											getNoon(atime, ltime, ntime.tm_year + 1900, ntime.tm_mon + 1,
												ntime.tm_mday - 1); // We're only interested in finding the date
											int year = ltime.tm_year + 1900;
											int mon = ltime.tm_mon + 1;
											int day = ltime.tm_mday;
											sprintf(szTmp, "%04d-%02d-%02d", year, mon, day);
											std::vector<std::vector<std::string>> result2;
											result2 = m_sql.safe_query(
												"SELECT Counter1, Counter2, Counter3, Counter4 FROM Multimeter_Calendar WHERE (DeviceRowID==%" PRIu64
												") AND (Date=='%q')",
												idx, szTmp);
											if (!result2.empty())
											{
												std::vector<std::string> sd = result2[0];
												firstUsage1 = std::stoll(sd[0]);
												firstDeliv1 = std::stoll(sd[1]);
												firstUsage2 = std::stoll(sd[2]);
												firstDeliv2 = std::stoll(sd[3]);
												lastDay = ntime.tm_mday;
											}
										}
									}
									lastUsage1 = actUsage1;
									lastUsage2 = actUsage2;
									lastDeliv1 = actDeliv1;
									lastDeliv2 = actDeliv2;
									lastTime = atime;
								}
								else
								{
									// this meter has no decimals, so return the use peaks
									root["result"][ii]["d"] = sd[6].substr(0, 16);

									if (sd[3] != "0")
										bHaveDeliverd = true;
									root["result"][ii]["v"] = sd[2];
									root["result"][ii]["r1"] = sd[3];
									ii++;
								}
							}
							if (bHaveDeliverd)
							{
								root["delivered"] = true;
							}
						}
					}
					else if (dType == pTypeAirQuality)
					{ // day
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;

						result = m_sql.safe_query("SELECT Value, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
						if (!result.empty())
						{
							int ii = 0;
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[1].substr(0, 16);
								root["result"][ii]["co2"] = sd[0];
								ii++;
							}
						}
					}
					else if ((dType == pTypeGeneral) && ((dSubType == sTypeSoilMoisture) || (dSubType == sTypeLeafWetness)))
					{ // day
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;

						result = m_sql.safe_query("SELECT Value, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
						if (!result.empty())
						{
							int ii = 0;
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[1].substr(0, 16);
								root["result"][ii]["v"] = sd[0];
								ii++;
							}
						}
					}
					else if (((dType == pTypeGeneral) && (dSubType == sTypeVisibility)) || ((dType == pTypeGeneral) && (dSubType == sTypeDistance)) ||
						((dType == pTypeGeneral) && (dSubType == sTypeSolarRadiation)) || ((dType == pTypeGeneral) && (dSubType == sTypeVoltage)) ||
						((dType == pTypeGeneral) && (dSubType == sTypeCurrent)) || ((dType == pTypeGeneral) && (dSubType == sTypePressure)) ||
						((dType == pTypeGeneral) && (dSubType == sTypeSoundLevel)))
					{ // day
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;
						float vdiv = 10.0F;
						if (((dType == pTypeGeneral) && (dSubType == sTypeVoltage)) || ((dType == pTypeGeneral) && (dSubType == sTypeCurrent)))
						{
							vdiv = 1000.0F;
						}
						result = m_sql.safe_query("SELECT Value, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
						if (!result.empty())
						{
							int ii = 0;
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[1].substr(0, 16);
								float fValue = float(atof(sd[0].c_str())) / vdiv;
								if (metertype == 1)
								{
									if ((dType == pTypeGeneral) && (dSubType == sTypeDistance))
										fValue *= 0.3937007874015748F; // inches
									else
										fValue *= 0.6214F; // miles
								}
								if ((dType == pTypeGeneral) && (dSubType == sTypeVoltage))
									sprintf(szTmp, "%.3f", fValue);
								else if ((dType == pTypeGeneral) && (dSubType == sTypeCurrent))
									sprintf(szTmp, "%.3f", fValue);
								else
									sprintf(szTmp, "%.1f", fValue);
								root["result"][ii]["v"] = szTmp;
								ii++;
							}
						}
					}
					else if ((dType == pTypeRFXSensor) && ((dSubType == sTypeRFXSensorAD) || (dSubType == sTypeRFXSensorVolt)))
					{ // day
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;

						result = m_sql.safe_query("SELECT Value, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
						if (!result.empty())
						{
							int ii = 0;
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[1].substr(0, 16);
								root["result"][ii]["v"] = sd[0];
								ii++;
							}
						}
					}
					else if (dType == pTypeLux)
					{ // day
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;

						result = m_sql.safe_query("SELECT Value, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
						if (!result.empty())
						{
							int ii = 0;
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[1].substr(0, 16);
								root["result"][ii]["lux"] = sd[0];
								ii++;
							}
						}
					}
					else if (dType == pTypeWEIGHT)
					{ // day
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;

						result = m_sql.safe_query("SELECT Value, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
						if (!result.empty())
						{
							int ii = 0;
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[1].substr(0, 16);
								sprintf(szTmp, "%.1f", m_sql.m_weightscale * atof(sd[0].c_str()) / 10.0F);
								root["result"][ii]["v"] = szTmp;
								ii++;
							}
						}
					}
					else if (dType == pTypeUsage)
					{ // day
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;

						result = m_sql.safe_query("SELECT Value, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
						if (!result.empty())
						{
							int ii = 0;
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[1].substr(0, 16);
								root["result"][ii]["u"] = atof(sd[0].c_str()) / 10.0F;
								ii++;
							}
						}
					}
					else if (dType == pTypeCURRENT)
					{
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;

						// CM113
						int displaytype = 0;
						int voltage = 230;
						m_sql.GetPreferencesVar("CM113DisplayType", displaytype);
						m_sql.GetPreferencesVar("ElectricVoltage", voltage);

						root["displaytype"] = displaytype;

						result = m_sql.safe_query("SELECT Value1, Value2, Value3, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
						if (!result.empty())
						{
							int ii = 0;
							bool bHaveL1 = false;
							bool bHaveL2 = false;
							bool bHaveL3 = false;
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[3].substr(0, 16);

								float fval1 = static_cast<float>(atof(sd[0].c_str()) / 10.0F);
								float fval2 = static_cast<float>(atof(sd[1].c_str()) / 10.0F);
								float fval3 = static_cast<float>(atof(sd[2].c_str()) / 10.0F);

								if (fval1 != 0)
									bHaveL1 = true;
								if (fval2 != 0)
									bHaveL2 = true;
								if (fval3 != 0)
									bHaveL3 = true;

								if (displaytype == 0)
								{
									sprintf(szTmp, "%.1f", fval1);
									root["result"][ii]["v1"] = szTmp;
									sprintf(szTmp, "%.1f", fval2);
									root["result"][ii]["v2"] = szTmp;
									sprintf(szTmp, "%.1f", fval3);
									root["result"][ii]["v3"] = szTmp;
								}
								else
								{
									sprintf(szTmp, "%d", int(fval1 * voltage));
									root["result"][ii]["v1"] = szTmp;
									sprintf(szTmp, "%d", int(fval2 * voltage));
									root["result"][ii]["v2"] = szTmp;
									sprintf(szTmp, "%d", int(fval3 * voltage));
									root["result"][ii]["v3"] = szTmp;
								}
								ii++;
							}
							if ((!bHaveL1) && (!bHaveL2) && (!bHaveL3))
							{
								root["haveL1"] = true; // show at least something
							}
							else
							{
								if (bHaveL1)
									root["haveL1"] = true;
								if (bHaveL2)
									root["haveL2"] = true;
								if (bHaveL3)
									root["haveL3"] = true;
							}
						}
					}
					else if (dType == pTypeCURRENTENERGY)
					{
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;

						// CM113
						int displaytype = 0;
						int voltage = 230;
						m_sql.GetPreferencesVar("CM113DisplayType", displaytype);
						m_sql.GetPreferencesVar("ElectricVoltage", voltage);

						root["displaytype"] = displaytype;

						result = m_sql.safe_query("SELECT Value1, Value2, Value3, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
						if (!result.empty())
						{
							int ii = 0;
							bool bHaveL1 = false;
							bool bHaveL2 = false;
							bool bHaveL3 = false;
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[3].substr(0, 16);

								float fval1 = static_cast<float>(atof(sd[0].c_str()) / 10.0F);
								float fval2 = static_cast<float>(atof(sd[1].c_str()) / 10.0F);
								float fval3 = static_cast<float>(atof(sd[2].c_str()) / 10.0F);

								if (fval1 != 0)
									bHaveL1 = true;
								if (fval2 != 0)
									bHaveL2 = true;
								if (fval3 != 0)
									bHaveL3 = true;

								if (displaytype == 0)
								{
									sprintf(szTmp, "%.1f", fval1);
									root["result"][ii]["v1"] = szTmp;
									sprintf(szTmp, "%.1f", fval2);
									root["result"][ii]["v2"] = szTmp;
									sprintf(szTmp, "%.1f", fval3);
									root["result"][ii]["v3"] = szTmp;
								}
								else
								{
									sprintf(szTmp, "%d", int(fval1 * voltage));
									root["result"][ii]["v1"] = szTmp;
									sprintf(szTmp, "%d", int(fval2 * voltage));
									root["result"][ii]["v2"] = szTmp;
									sprintf(szTmp, "%d", int(fval3 * voltage));
									root["result"][ii]["v3"] = szTmp;
								}
								ii++;
							}
							if ((!bHaveL1) && (!bHaveL2) && (!bHaveL3))
							{
								root["haveL1"] = true; // show at least something
							}
							else
							{
								if (bHaveL1)
									root["haveL1"] = true;
								if (bHaveL2)
									root["haveL2"] = true;
								if (bHaveL3)
									root["haveL3"] = true;
							}
						}
					}
					else if ((dType == pTypeENERGY) || (dType == pTypePOWER) || (dType == pTypeYouLess) || ((dType == pTypeGeneral) && (dSubType == sTypeKwh)))
					{
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;
						root["ValueQuantity"] = options["ValueQuantity"];
						root["ValueUnits"] = options["ValueUnits"];
						root["Divider"] = divider;

						// First check if we had any usage in the short log, if not, its probably a meter without usage
						bool bHaveUsage = true;
						result = m_sql.safe_query("SELECT MIN([Usage]), MAX([Usage]) FROM %s WHERE (DeviceRowID==%" PRIu64 ")", dbasetable.c_str(), idx);
						if (!result.empty())
						{
							int64_t minValue = std::stoll(result[0][0]);
							int64_t maxValue = std::stoll(result[0][1]);

							if ((minValue == 0) && (maxValue == 0))
							{
								bHaveUsage = false;
							}
						}

						int ii = 0;
						result = m_sql.safe_query("SELECT Value,[Usage], Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);

						int method = 0;
						std::string sMethod = request::findValue(&req, "method");
						if (!sMethod.empty())
							method = atoi(sMethod.c_str());
						if (bHaveUsage == false)
							method = 0;

						if ((dType == pTypeYouLess) && ((metertype == MTYPE_ENERGY) || (metertype == MTYPE_ENERGY_GENERATED)))
							method = 1;

						double dividerForQuantity = divider; // kWh, m3, l
						double dividerForRate = divider; // Watt, m3/hour, l/hour
						if (method != 0)
						{
							// realtime graph
							if ((dType == pTypeENERGY) || (dType == pTypePOWER))
							{
								dividerForRate /= 100.0F;
							}
						}

						root["method"] = method;
						bool bHaveFirstValue = false;
						bool bHaveFirstRealValue = false;
						int64_t ulFirstRealValue = 0;
						int64_t ulFirstValue = 0;
						int64_t ulLastValue = 0;
						std::string LastDateTime;

						if (!result.empty())
						{
							for (auto itt = result.begin(); itt != result.end(); ++itt)
							{
								std::vector<std::string> sd = *itt;

								// If method == 1, provide BOTH hourly and instant usage for combined graph
								{
									// bars / hour
									std::string actDateTimeHour = sd[2].substr(0, 13);
									int64_t actValue = std::stoll(sd[0]); // actual energy value

									ulLastValue = actValue;

									if (ulLastValue < ulFirstValue)
									{
										if (ulFirstValue - ulLastValue > 20000)
										{
											//probably a meter/counter turnover
											ulFirstValue = ulFirstRealValue = ulLastValue;
											LastDateTime = actDateTimeHour;
										}
									}

									if (actDateTimeHour != LastDateTime || ((method == 1) && (itt + 1 == result.end())))
									{
										if (bHaveFirstValue)
										{
											// root["result"][ii]["d"] = LastDateTime + (method == 1 ? ":30" : ":00");
											//^^ not necessarily bad, but is currently inconsistent with all other day graphs
											root["result"][ii]["d"] = LastDateTime + ":00";

											int64_t ulTotalValue = ulLastValue - ulFirstValue;
											if (ulTotalValue == 0)
											{
												// Could be the P1 Gas Meter, only transmits one every 1 a 2 hours
												ulTotalValue = ulLastValue - ulFirstRealValue;
											}
											ulFirstRealValue = ulLastValue;
											double TotalValue = double(ulTotalValue);
											double dividerHere = method == 1 ? dividerForQuantity : dividerForRate;
											switch (metertype)
											{
											case MTYPE_ENERGY:
											case MTYPE_ENERGY_GENERATED:
												sprintf(szTmp, "%.3f", (TotalValue / dividerHere) * 1000.0); // from kWh -> Watt
												break;
											case MTYPE_GAS:
												sprintf(szTmp, "%.3f", TotalValue / dividerHere);
												break;
											case MTYPE_WATER:
												sprintf(szTmp, "%.3f", TotalValue / dividerHere);
												break;
											case MTYPE_COUNTER:
												sprintf(szTmp, "%.10g", TotalValue / dividerHere);
												break;
											default:
												strcpy(szTmp, "0");
												break;
											}
											root["result"][ii][method == 1 ? "eu" : "v"] = szTmp;
											ii++;
										}
										LastDateTime = actDateTimeHour;
										bHaveFirstValue = false;
									}
									if (!bHaveFirstValue)
									{
										ulFirstValue = ulLastValue;
										bHaveFirstValue = true;
									}
									if (!bHaveFirstRealValue)
									{
										bHaveFirstRealValue = true;
										ulFirstRealValue = ulLastValue;
									}
								}

								if (method == 1)
								{
									int64_t actValue = std::stoll(sd[1]);

									root["result"][ii]["d"] = sd[2].substr(0, 16);

									double TotalValue = double(actValue);
									if ((dType == pTypeGeneral) && (dSubType == sTypeKwh))
										TotalValue /= 10.0F;
									switch (metertype)
									{
									case MTYPE_ENERGY:
									case MTYPE_ENERGY_GENERATED:
										sprintf(szTmp, "%.3f", (TotalValue / dividerForRate) * 1000.0); // from kWh -> Watt
										break;
									case MTYPE_GAS:
										sprintf(szTmp, "%.2f", TotalValue / dividerForRate);
										break;
									case MTYPE_WATER:
										sprintf(szTmp, "%.3f", TotalValue / dividerForRate);
										break;
									case MTYPE_COUNTER:
										sprintf(szTmp, "%.10g", TotalValue / dividerForRate);
										break;
									default:
										strcpy(szTmp, "0");
										break;
									}
									root["result"][ii]["v"] = szTmp;
									ii++;
								}
							}
						}
					}
					else
					{
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;
						root["ValueQuantity"] = options["ValueQuantity"];
						root["ValueUnits"] = options["ValueUnits"];
						root["Divider"] = divider;

						int ii = 0;

						bool bHaveFirstValue = false;
						bool bHaveFirstRealValue = false;
						int64_t ulFirstValue = 0;
						int64_t ulRealFirstValue = 0;
						int lastDay = 0;
						std::string szLastDateTimeHour;
						std::string szActDateTimeHour;
						std::string szlastDateTime;
						int64_t ulLastValue = 0;

						int lastHour = 0;
						time_t lastTime = 0;

						int method = 0;
						std::string sMethod = request::findValue(&req, "method");
						if (!sMethod.empty())
							method = atoi(sMethod.c_str());

						if (bIsManagedCounter)
						{
							result = m_sql.safe_query("SELECT Usage, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
							bHaveFirstValue = true;
							bHaveFirstRealValue = true;
							method = 1;
						}
						else
						{
							result = m_sql.safe_query("SELECT Value, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
						}

						if (!result.empty())
						{
							double lastUsageValue = 0;

							for (const auto& sd : result)
							{
								if (method == 0)
								{
									// bars / hour
									int64_t actValue = std::stoll(sd[0]);
									szlastDateTime = sd[1].substr(0, 16);
									szActDateTimeHour = sd[1].substr(0, 13) + ":00";

									struct tm ntime;
									time_t atime;
									ParseSQLdatetime(atime, ntime, sd[1], -1);

									if (actValue < ulFirstValue)
									{
										if (ulRealFirstValue - actValue > 20000)
										{
											//Assume ,eter/counter turnover
											ulFirstValue = ulRealFirstValue = actValue;
											lastHour = ntime.tm_hour;
										}
									}

									if (lastHour != ntime.tm_hour)
									{
										if (lastDay != ntime.tm_mday)
										{
											lastDay = ntime.tm_mday;
											ulRealFirstValue = actValue;
										}

										if (bHaveFirstValue)
										{
											root["result"][ii]["d"] = szLastDateTimeHour;

											// float TotalValue = float(actValue - ulFirstValue);

											// prevents graph from going crazy if the meter counter resets
											// removed because it breaks  negative increments
											double TotalValue=double(actValue - ulFirstValue);
											//if (actValue < ulFirstValue) TotalValue=actValue;

											// if (TotalValue != 0)
											{
												switch (metertype)
												{
												case MTYPE_ENERGY:
												case MTYPE_ENERGY_GENERATED:
													sprintf(szTmp, "%.3f", (TotalValue / divider) * 1000.0); // from kWh -> Watt
													break;
												case MTYPE_GAS:
													sprintf(szTmp, "%.3f", TotalValue / divider);
													break;
												case MTYPE_WATER:
													sprintf(szTmp, "%.3f", TotalValue / divider);
													break;
												case MTYPE_COUNTER:
													sprintf(szTmp, "%.10g", TotalValue / divider);
													break;
												default:
													strcpy(szTmp, "0");
													break;
												}
												root["result"][ii]["v"] = szTmp;

												if (!bIsManagedCounter)
												{
													double usageValue = lastUsageValue;

													switch (metertype)
													{
													case MTYPE_ENERGY:
													case MTYPE_ENERGY_GENERATED:
														sprintf(szTmp, "%.3f", usageValue / divider);
														break;
													case MTYPE_GAS:
														sprintf(szTmp, "%.3f", usageValue / divider);
														break;
													case MTYPE_WATER:
														sprintf(szTmp, "%g", usageValue);
														break;
													case MTYPE_COUNTER:
														sprintf(szTmp, "%.3f", usageValue / divider);
														break;
													}
													root["result"][ii]["mu"] = szTmp;
												}
												ii++;
											}
										}
										if (!bIsManagedCounter)
										{
											ulFirstValue = actValue;
										}
										lastHour = ntime.tm_hour;
									}

									if (!bHaveFirstValue)
									{
										bHaveFirstValue = true;
										lastHour = ntime.tm_hour;
										ulFirstValue = actValue;
										ulRealFirstValue = actValue;
										lastDay = ntime.tm_mday;

										if (!((ntime.tm_hour == 0) && (ntime.tm_min == 0)))
										{
											struct tm ltime;
											localtime_r(&atime, &tm1);
											getNoon(atime, ltime, ntime.tm_year + 1900, ntime.tm_mon + 1,
												ntime.tm_mday - 1); // We're only interested in finding the date
											int year = ltime.tm_year + 1900;
											int mon = ltime.tm_mon + 1;
											int day = ltime.tm_mday;
											sprintf(szTmp, "%04d-%02d-%02d", year, mon, day);
											std::vector<std::vector<std::string>> result2;
											result2 = m_sql.safe_query(
												"SELECT Counter FROM %s_Calendar WHERE (DeviceRowID==%" PRIu64
												") AND (Date=='%q')",
												dbasetable.c_str(), idx, szTmp);
											if (!result2.empty())
											{
												std::vector<std::string> sd = result2[0];
												ulRealFirstValue = std::stoll(sd[0]);
												lastDay = ntime.tm_mday;
											}
										}
									}
									szLastDateTimeHour = szActDateTimeHour;
									lastUsageValue = (double)(actValue - ulRealFirstValue);
									ulLastValue = actValue;
								}
								else
								{
									// realtime graph
									int64_t actValue = std::stoll(sd[0]);

									std::string stime = sd[1];
									struct tm ntime;
									time_t atime;
									ParseSQLdatetime(atime, ntime, stime, -1);
									if (bHaveFirstRealValue)
									{
										int64_t curValue;
										float tlaps = 1;

										if (!bIsManagedCounter)
										{
											curValue = actValue - ulLastValue;
											float tdiff;
											tdiff = static_cast<float>(difftime(atime, lastTime));
											if (tdiff == 0)
												tdiff = 1;
											tlaps = 3600.0F / tdiff;
										}
										else
										{
											curValue = actValue;
										}
										
										curValue *= int(tlaps);

										root["result"][ii]["d"] = sd[1].substr(0, 16);

										double TotalValue = double(curValue);
										// if (TotalValue != 0)
										{
											switch (metertype)
											{
											case MTYPE_ENERGY:
											case MTYPE_ENERGY_GENERATED:
												sprintf(szTmp, "%.3f", (TotalValue / divider) * 1000.0); // from kWh -> Watt
												break;
											case MTYPE_GAS:
												sprintf(szTmp, "%.2f", TotalValue / divider);
												break;
											case MTYPE_WATER:
												sprintf(szTmp, "%.3f", TotalValue / divider);
												break;
											case MTYPE_COUNTER:
												sprintf(szTmp, "%.10g", TotalValue / divider);
												break;
											default:
												strcpy(szTmp, "0");
												break;
											}
											root["result"][ii]["v"] = szTmp;
											ii++;
										}
									}
									else
										bHaveFirstRealValue = true;
									if (!bIsManagedCounter)
									{
										ulLastValue = actValue;
									}
									lastTime = atime;
								}
							}
						}
						if ((!bIsManagedCounter) && (bHaveFirstValue) && (method == 0))
						{
							// add last value
							root["result"][ii]["d"] = szLastDateTimeHour;

							int64_t ulTotalValue = ulLastValue - ulFirstValue;

							double TotalValue = double(ulTotalValue);

							// if (TotalValue != 0)
							{
								switch (metertype)
								{
								case MTYPE_ENERGY:
								case MTYPE_ENERGY_GENERATED:
									sprintf(szTmp, "%.3f", (TotalValue / divider) * 1000.0); // from kWh -> Watt
									break;
								case MTYPE_GAS:
									sprintf(szTmp, "%.3f", TotalValue / divider);
									break;
								case MTYPE_WATER:
									sprintf(szTmp, "%.3f", TotalValue / divider);
									break;
								case MTYPE_COUNTER:
									sprintf(szTmp, "%.10g", TotalValue / divider);
									break;
								default:
									strcpy(szTmp, "0");
									break;
								}
								root["result"][ii]["v"] = szTmp;

								if (!bIsManagedCounter)
								{
									double usageValue = (double)(ulLastValue - ulRealFirstValue);
									switch (metertype)
									{
									case MTYPE_ENERGY:
									case MTYPE_ENERGY_GENERATED:
										sprintf(szTmp, "%.3f", usageValue / divider);
										break;
									case MTYPE_GAS:
										sprintf(szTmp, "%.3f", usageValue / divider);
										break;
									case MTYPE_WATER:
										sprintf(szTmp, "%.3f", usageValue);
										break;
									case MTYPE_COUNTER:
										sprintf(szTmp, "%.3f", usageValue / divider);
										break;
									}
									root["result"][ii]["mu"] = szTmp;
								}
								ii++;
							}
						}
					}
				}
				else if (sensor == "uv")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					result = m_sql.safe_query("SELECT Level, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
					if (!result.empty())
					{
						int ii = 0;
						for (const auto& sd : result)
						{
							root["result"][ii]["d"] = sd[1].substr(0, 16);
							root["result"][ii]["uvi"] = sd[0];
							ii++;
						}
					}
				}
				else if (sensor == "rain")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					int WorkingHour = -1;
					std::string WorkingHourDate;
					float WorkingHourStartValue = -1;

					float LastValue = -1;
					std::string LastDate;

					result = m_sql.safe_query("SELECT Total, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
					if (!result.empty())
					{
						int ii = 0;
						for (const auto& sd : result)
						{
							float ActTotal = static_cast<float>(atof(sd[0].c_str()));
							int Hour = atoi(sd[1].substr(11, 2).c_str());
							if (Hour != WorkingHour)
							{
								if (WorkingHour != -1)
								{
									//Finish current hour
									root["result"][ii]["d"] = WorkingHourDate.substr(0, 14) + "00";
									double mmval = ActTotal - WorkingHourStartValue;
									mmval *= AddjMulti;
									sprintf(szTmp, "%.1f", mmval);
									root["result"][ii]["mm"] = szTmp;
									ii++;
								}
								WorkingHour = Hour;
								WorkingHourStartValue = ActTotal;
								WorkingHourDate = sd[1];
							}
							LastValue = ActTotal;
							LastDate = sd[1];
						}
						//Add last value
						result = m_sql.safe_query("SELECT sValue, LastUpdate FROM DeviceStatus WHERE (ID==%" PRIu64 ")", idx);
						if (!result.empty())
						{
							std::string sValue = result[0][0];
							std::vector<std::string> results;
							StringSplit(sValue, ";", results);
							if (results.size() == 2)
							{
								float ActTotal = static_cast<float>(atof(results[1].c_str()));
								if (ActTotal > LastValue)
									LastValue = ActTotal;
							}
						}
						double mmval = LastValue - WorkingHourStartValue;
						if (mmval != 0)
						{
							root["result"][ii]["d"] = WorkingHourDate.substr(0, 14) + "00";
							mmval *= AddjMulti;
							sprintf(szTmp, "%.1f", mmval);
							root["result"][ii]["mm"] = szTmp;
							ii++;
						}
					}
				}
				else if (sensor == "wind")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					result = m_sql.safe_query("SELECT Direction, Speed, Gust, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
					if (!result.empty())
					{
						int ii = 0;
						for (const auto& sd : result)
						{
							root["result"][ii]["d"] = sd[3].substr(0, 16);
							root["result"][ii]["di"] = sd[0];

							int intSpeed = atoi(sd[1].c_str());
							int intGust = atoi(sd[2].c_str());

							if (m_sql.m_windunit != WINDUNIT_Beaufort)
							{
								sprintf(szTmp, "%.1f", float(intSpeed) * m_sql.m_windscale);
								root["result"][ii]["sp"] = szTmp;
								sprintf(szTmp, "%.1f", float(intGust) * m_sql.m_windscale);
								root["result"][ii]["gu"] = szTmp;
							}
							else
							{
								float windspeedms = float(intSpeed) * 0.1F;
								float windgustms = float(intGust) * 0.1F;
								sprintf(szTmp, "%d", MStoBeaufort(windspeedms));
								root["result"][ii]["sp"] = szTmp;
								sprintf(szTmp, "%d", MStoBeaufort(windgustms));
								root["result"][ii]["gu"] = szTmp;
							}
							ii++;
						}
					}
				}
				else if (sensor == "winddir")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					result = m_sql.safe_query("SELECT Direction, Speed, Gust FROM %s WHERE (DeviceRowID==%" PRIu64 ") ORDER BY Date ASC", dbasetable.c_str(), idx);
					if (!result.empty())
					{
						std::map<int, int> _directions;
						std::array<std::array<int, 8>, 17> wdirtabletemp = {};
						std::string szLegendLabels[7];
						int ii = 0;

						int totalvalues = 0;
						// init dir list
						int idir;
						for (idir = 0; idir < 360 + 1; idir++)
							_directions[idir] = 0;

						if (m_sql.m_windunit == WINDUNIT_MS)
						{
							szLegendLabels[0] = "&lt; 0.5 " + m_sql.m_windsign;
							szLegendLabels[1] = "0.5-2 " + m_sql.m_windsign;
							szLegendLabels[2] = "2-4 " + m_sql.m_windsign;
							szLegendLabels[3] = "4-6 " + m_sql.m_windsign;
							szLegendLabels[4] = "6-8 " + m_sql.m_windsign;
							szLegendLabels[5] = "8-10 " + m_sql.m_windsign;
							szLegendLabels[6] = "&gt; 10" + m_sql.m_windsign;
						}
						else if (m_sql.m_windunit == WINDUNIT_KMH)
						{
							szLegendLabels[0] = "&lt; 2 " + m_sql.m_windsign;
							szLegendLabels[1] = "2-4 " + m_sql.m_windsign;
							szLegendLabels[2] = "4-6 " + m_sql.m_windsign;
							szLegendLabels[3] = "6-10 " + m_sql.m_windsign;
							szLegendLabels[4] = "10-20 " + m_sql.m_windsign;
							szLegendLabels[5] = "20-36 " + m_sql.m_windsign;
							szLegendLabels[6] = "&gt; 36" + m_sql.m_windsign;
						}
						else if (m_sql.m_windunit == WINDUNIT_MPH)
						{
							szLegendLabels[0] = "&lt; 3 " + m_sql.m_windsign;
							szLegendLabels[1] = "3-7 " + m_sql.m_windsign;
							szLegendLabels[2] = "7-12 " + m_sql.m_windsign;
							szLegendLabels[3] = "12-18 " + m_sql.m_windsign;
							szLegendLabels[4] = "18-24 " + m_sql.m_windsign;
							szLegendLabels[5] = "24-46 " + m_sql.m_windsign;
							szLegendLabels[6] = "&gt; 46" + m_sql.m_windsign;
						}
						else if (m_sql.m_windunit == WINDUNIT_Knots)
						{
							szLegendLabels[0] = "&lt; 3 " + m_sql.m_windsign;
							szLegendLabels[1] = "3-7 " + m_sql.m_windsign;
							szLegendLabels[2] = "7-17 " + m_sql.m_windsign;
							szLegendLabels[3] = "17-27 " + m_sql.m_windsign;
							szLegendLabels[4] = "27-34 " + m_sql.m_windsign;
							szLegendLabels[5] = "34-41 " + m_sql.m_windsign;
							szLegendLabels[6] = "&gt; 41" + m_sql.m_windsign;
						}
						else if (m_sql.m_windunit == WINDUNIT_Beaufort)
						{
							szLegendLabels[0] = "&lt; 2 " + m_sql.m_windsign;
							szLegendLabels[1] = "2-4 " + m_sql.m_windsign;
							szLegendLabels[2] = "4-6 " + m_sql.m_windsign;
							szLegendLabels[3] = "6-8 " + m_sql.m_windsign;
							szLegendLabels[4] = "8-10 " + m_sql.m_windsign;
							szLegendLabels[5] = "10-12 " + m_sql.m_windsign;
							szLegendLabels[6] = "&gt; 12" + m_sql.m_windsign;
						}
						else
						{
							// Todo !
							szLegendLabels[0] = "&lt; 0.5 " + m_sql.m_windsign;
							szLegendLabels[1] = "0.5-2 " + m_sql.m_windsign;
							szLegendLabels[2] = "2-4 " + m_sql.m_windsign;
							szLegendLabels[3] = "4-6 " + m_sql.m_windsign;
							szLegendLabels[4] = "6-8 " + m_sql.m_windsign;
							szLegendLabels[5] = "8-10 " + m_sql.m_windsign;
							szLegendLabels[6] = "&gt; 10" + m_sql.m_windsign;
						}

						for (const auto& sd : result)
						{
							float fdirection = static_cast<float>(atof(sd[0].c_str()));
							if (fdirection >= 360)
								fdirection = 0;
							int direction = int(fdirection);
							float speedOrg = static_cast<float>(atof(sd[1].c_str()));
							float gustOrg = static_cast<float>(atof(sd[2].c_str()));
							if ((gustOrg == 0) && (speedOrg != 0))
								gustOrg = speedOrg;
							if (gustOrg == 0)
								continue; // no direction if wind is still
							// float speed = speedOrg * m_sql.m_windscale;
							float gust = gustOrg * m_sql.m_windscale;
							int bucket = int(fdirection / 22.5F);

							int speedpos = 0;

							if (m_sql.m_windunit == WINDUNIT_MS)
							{
								if (gust < 0.5F)
									speedpos = 0;
								else if (gust < 2.0F)
									speedpos = 1;
								else if (gust < 4.0F)
									speedpos = 2;
								else if (gust < 6.0F)
									speedpos = 3;
								else if (gust < 8.0F)
									speedpos = 4;
								else if (gust < 10.0F)
									speedpos = 5;
								else
									speedpos = 6;
							}
							else if (m_sql.m_windunit == WINDUNIT_KMH)
							{
								if (gust < 2.0F)
									speedpos = 0;
								else if (gust < 4.0F)
									speedpos = 1;
								else if (gust < 6.0F)
									speedpos = 2;
								else if (gust < 10.0F)
									speedpos = 3;
								else if (gust < 20.0F)
									speedpos = 4;
								else if (gust < 36.0F)
									speedpos = 5;
								else
									speedpos = 6;
							}
							else if (m_sql.m_windunit == WINDUNIT_MPH)
							{
								if (gust < 3.0F)
									speedpos = 0;
								else if (gust < 7.0F)
									speedpos = 1;
								else if (gust < 12.0F)
									speedpos = 2;
								else if (gust < 18.0F)
									speedpos = 3;
								else if (gust < 24.0F)
									speedpos = 4;
								else if (gust < 46.0F)
									speedpos = 5;
								else
									speedpos = 6;
							}
							else if (m_sql.m_windunit == WINDUNIT_Knots)
							{
								if (gust < 3.0F)
									speedpos = 0;
								else if (gust < 7.0F)
									speedpos = 1;
								else if (gust < 17.0F)
									speedpos = 2;
								else if (gust < 27.0F)
									speedpos = 3;
								else if (gust < 34.0F)
									speedpos = 4;
								else if (gust < 41.0F)
									speedpos = 5;
								else
									speedpos = 6;
							}
							else if (m_sql.m_windunit == WINDUNIT_Beaufort)
							{
								float gustms = gustOrg * 0.1F;
								int iBeaufort = MStoBeaufort(gustms);
								if (iBeaufort < 2)
									speedpos = 0;
								else if (iBeaufort < 4)
									speedpos = 1;
								else if (iBeaufort < 6)
									speedpos = 2;
								else if (iBeaufort < 8)
									speedpos = 3;
								else if (iBeaufort < 10)
									speedpos = 4;
								else if (iBeaufort < 12)
									speedpos = 5;
								else
									speedpos = 6;
							}
							else
							{
								// Still todo !
								if (gust < 0.5F)
									speedpos = 0;
								else if (gust < 2.0F)
									speedpos = 1;
								else if (gust < 4.0F)
									speedpos = 2;
								else if (gust < 6.0F)
									speedpos = 3;
								else if (gust < 8.0F)
									speedpos = 4;
								else if (gust < 10.0F)
									speedpos = 5;
								else
									speedpos = 6;
							}
							wdirtabletemp[bucket][speedpos]++;
							_directions[direction]++;
							totalvalues++;
						}

						for (int jj = 0; jj < 7; jj++)
						{
							root["result_speed"][jj]["label"] = szLegendLabels[jj];

							for (ii = 0; ii < 16; ii++)
							{
								float svalue = 0;
								if (totalvalues > 0)
								{
									svalue = (100.0F / totalvalues) * wdirtabletemp[ii][jj];
								}
								sprintf(szTmp, "%.2f", svalue);
								root["result_speed"][jj]["sp"][ii] = szTmp;
							}
						}
						ii = 0;
						for (idir = 0; idir < 360 + 1; idir++)
						{
							if (_directions[idir] != 0)
							{
								root["result"][ii]["dig"] = idir;
								float percentage = 0;
								if (totalvalues > 0)
								{
									percentage = (float(100.0 / float(totalvalues)) * float(_directions[idir]));
								}
								sprintf(szTmp, "%.2f", percentage);
								root["result"][ii]["div"] = szTmp;
								ii++;
							}
						}
					}
				}

			} // day
			else if (srange == "week")
			{
				if (sensor == "rain")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					char szDateStart[40];
					char szDateEnd[40];
					sprintf(szDateEnd, "%04d-%02d-%02d", tm1.tm_year + 1900, tm1.tm_mon + 1, tm1.tm_mday);

					// Subtract one week
					time_t weekbefore;
					struct tm tm2;
					getNoon(weekbefore, tm2, tm1.tm_year + 1900, tm1.tm_mon + 1, tm1.tm_mday - 7); // We only want the date
					sprintf(szDateStart, "%04d-%02d-%02d", tm2.tm_year + 1900, tm2.tm_mon + 1, tm2.tm_mday);

					result = m_sql.safe_query("SELECT Total, Rate, Date FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
						dbasetable.c_str(), idx, szDateStart, szDateEnd);
					int ii = 0;
					if (!result.empty())
					{
						for (const auto& sd : result)
						{
							root["result"][ii]["d"] = sd[2].substr(0, 16);
							double mmval = atof(sd[0].c_str());
							mmval *= AddjMulti;
							sprintf(szTmp, "%.1f", mmval);
							root["result"][ii]["mm"] = szTmp;
							ii++;
						}
					}
					// add today (have to calculate it)
					if (dSubType == sTypeRAINWU || dSubType == sTypeRAINByRate)
					{
						result = m_sql.safe_query("SELECT Total, Total, Rate FROM Rain WHERE (DeviceRowID=%" PRIu64 " AND Date>='%q') ORDER BY ROWID DESC LIMIT 1", idx,
							szDateEnd);
					}
					else
					{
						result = m_sql.safe_query("SELECT MIN(Total), MAX(Total), MAX(Rate) FROM Rain WHERE (DeviceRowID=%" PRIu64 " AND Date>='%q')", idx, szDateEnd);
					}
					if (!result.empty())
					{
						std::vector<std::string> sd = result[0];

						float total_min = static_cast<float>(atof(sd[0].c_str()));
						float total_max = static_cast<float>(atof(sd[1].c_str()));
						// int rate = atoi(sd[2].c_str());

						double total_real = 0;
						if (dSubType == sTypeRAINWU || dSubType == sTypeRAINByRate)
						{
							total_real = total_max;
						}
						else
						{
							total_real = total_max - total_min;
						}
						total_real *= AddjMulti;
						sprintf(szTmp, "%.1f", total_real);
						root["result"][ii]["d"] = szDateEnd;
						root["result"][ii]["mm"] = szTmp;
						ii++;
					}
				}
				else if (sensor == "counter")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;
					root["ValueQuantity"] = options["ValueQuantity"];
					root["ValueUnits"] = options["ValueUnits"];
					root["Divider"] = divider;

					char szDateStart[40];
					char szDateEnd[40];
					sprintf(szDateEnd, "%04d-%02d-%02d", tm1.tm_year + 1900, tm1.tm_mon + 1, tm1.tm_mday);

					// Subtract one week
					time_t weekbefore;
					struct tm tm2;
					getNoon(weekbefore, tm2, tm1.tm_year + 1900, tm1.tm_mon + 1, tm1.tm_mday - 7); // We only want the date
					sprintf(szDateStart, "%04d-%02d-%02d", tm2.tm_year + 1900, tm2.tm_mon + 1, tm2.tm_mday);

					int ii = 0;
					if (dType == pTypeP1Power)
					{
						result = m_sql.safe_query("SELECT Value1,Value2,Value5,Value6,Date FROM %s WHERE (DeviceRowID==%" PRIu64
							" AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
							dbasetable.c_str(), idx, szDateStart, szDateEnd);
						if (!result.empty())
						{
							bool bHaveDeliverd = false;
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[4].substr(0, 16);
								std::string szValueUsage1 = sd[0];
								std::string szValueDeliv1 = sd[1];
								std::string szValueUsage2 = sd[2];
								std::string szValueDeliv2 = sd[3];

								float fUsage1 = (float)(atof(szValueUsage1.c_str()));
								float fUsage2 = (float)(atof(szValueUsage2.c_str()));
								float fDeliv1 = (float)(atof(szValueDeliv1.c_str()));
								float fDeliv2 = (float)(atof(szValueDeliv2.c_str()));

								fDeliv1 = (fDeliv1 < 10) ? 0 : fDeliv1;
								fDeliv2 = (fDeliv2 < 10) ? 0 : fDeliv2;

								if ((fDeliv1 != 0) || (fDeliv2 != 0))
									bHaveDeliverd = true;
								sprintf(szTmp, "%.3f", fUsage1 / divider);
								root["result"][ii]["v"] = szTmp;
								sprintf(szTmp, "%.3f", fUsage2 / divider);
								root["result"][ii]["v2"] = szTmp;
								sprintf(szTmp, "%.3f", fDeliv1 / divider);
								root["result"][ii]["r1"] = szTmp;
								sprintf(szTmp, "%.3f", fDeliv2 / divider);
								root["result"][ii]["r2"] = szTmp;
								ii++;
							}
							if (bHaveDeliverd)
							{
								root["delivered"] = true;
							}
						}
					}
					else
					{
						result = m_sql.safe_query("SELECT Value, Date FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
							dbasetable.c_str(), idx, szDateStart, szDateEnd);
						if (!result.empty())
						{
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[1].substr(0, 16);
								std::string szValue = sd[0];
								switch (metertype)
								{
								case MTYPE_ENERGY:
								case MTYPE_ENERGY_GENERATED:
									sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
									szValue = szTmp;
									break;
								case MTYPE_GAS:
									sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
									szValue = szTmp;
									break;
								case MTYPE_WATER:
									sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
									szValue = szTmp;
									break;
								case MTYPE_COUNTER:
									sprintf(szTmp, "%.10g", atof(szValue.c_str()) / divider);
									szValue = szTmp;
									break;
								default:
									szValue = "0";
									break;
								}
								root["result"][ii]["v"] = szValue;
								ii++;
							}
						}
					}
					// add today (have to calculate it)
					if (dType == pTypeP1Power)
					{
						result = m_sql.safe_query("SELECT MIN(Value1), MAX(Value1), MIN(Value2), MAX(Value2),MIN(Value5), MAX(Value5), MIN(Value6), MAX(Value6) FROM "
							"MultiMeter WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q')",
							idx, szDateEnd);
						if (!result.empty())
						{
							std::vector<std::string> sd = result[0];

							uint64_t total_min_usage_1 = std::stoull(sd[0]);
							uint64_t total_max_usage_1 = std::stoull(sd[1]);
							uint64_t total_min_usage_2 = std::stoull(sd[4]);
							uint64_t total_max_usage_2 = std::stoull(sd[5]);
							uint64_t total_real_usage_1, total_real_usage_2;
							uint64_t total_min_deliv_1 = std::stoull(sd[2]);
							uint64_t total_max_deliv_1 = std::stoull(sd[3]);
							uint64_t total_min_deliv_2 = std::stoull(sd[6]);
							uint64_t total_max_deliv_2 = std::stoull(sd[7]);
							uint64_t total_real_deliv_1, total_real_deliv_2;

							bool bHaveDeliverd = false;

							total_real_usage_1 = total_max_usage_1 - total_min_usage_1;
							total_real_usage_2 = total_max_usage_2 - total_min_usage_2;

							total_real_deliv_1 = total_max_deliv_1 - total_min_deliv_1;
							total_real_deliv_2 = total_max_deliv_2 - total_min_deliv_2;
							if ((total_real_deliv_1 != 0) || (total_real_deliv_2 != 0))
								bHaveDeliverd = true;

							root["result"][ii]["d"] = szDateEnd;

							sprintf(szTmp, "%" PRIu64, total_real_usage_1);
							std::string szValue = szTmp;
							sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
							root["result"][ii]["v"] = szTmp;

							sprintf(szTmp, "%" PRIu64, total_real_usage_2);
							szValue = szTmp;
							sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
							root["result"][ii]["v2"] = szTmp;

							sprintf(szTmp, "%" PRIu64, total_real_deliv_1);
							szValue = szTmp;
							sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
							root["result"][ii]["r1"] = szTmp;

							sprintf(szTmp, "%" PRIu64, total_real_deliv_2);
							szValue = szTmp;
							sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
							root["result"][ii]["r2"] = szTmp;

							ii++;
							if (bHaveDeliverd)
							{
								root["delivered"] = true;
							}
						}
					}
					else if (!bIsManagedCounter)
					{
						// get the first value of the day
						result = m_sql.safe_query("SELECT Value FROM Meter WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q') ORDER BY Date ASC LIMIT 1", idx, szDateEnd);
						if (!result.empty())
						{
							std::vector<std::string> sd = result[0];

							int64_t total_min = std::stoll(sd[0]);
							int64_t total_max = total_min;
							int64_t total_real;

							// get the last value of the day
							result = m_sql.safe_query("SELECT Value FROM Meter WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q') ORDER BY Date DESC LIMIT 1", idx, szDateEnd);
							if (!result.empty())
							{
								std::vector<std::string> sd = result[0];
								total_max = std::stoull(sd[0].c_str());
							}

							total_real = total_max - total_min;
							sprintf(szTmp, "%" PRId64, total_real);

							std::string szValue = szTmp;
							switch (metertype)
							{
							case MTYPE_ENERGY:
							case MTYPE_ENERGY_GENERATED:
								sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
								szValue = szTmp;
								break;
							case MTYPE_GAS:
								sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
								szValue = szTmp;
								break;
							case MTYPE_WATER:
								sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
								szValue = szTmp;
								break;
							case MTYPE_COUNTER:
								sprintf(szTmp, "%.10g", atof(szValue.c_str()) / divider);
								szValue = szTmp;
								break;
							default:
								szValue = "0";
								break;
							}

							root["result"][ii]["d"] = szDateEnd;
							root["result"][ii]["v"] = szValue;
							ii++;
						}
					}
				}
			} // week
			else if (srange == "month" || srange == "year" || !sgroupby.empty())
			{
				char szDateStart[40];
				char szDateEnd[40];
				char szDateStartPrev[40];
				char szDateEndPrev[40];

				std::string sactmonth = request::findValue(&req, "actmonth");
				std::string sactyear = request::findValue(&req, "actyear");

				int actMonth = atoi(sactmonth.c_str());
				int actYear = atoi(sactyear.c_str());

				if ((!sactmonth.empty()) && (!sactyear.empty()))
				{
					sprintf(szDateStart, "%04d-%02d-%02d", actYear, actMonth, 1);
					sprintf(szDateStartPrev, "%04d-%02d-%02d", actYear - 1, actMonth, 1);
					actMonth++;
					if (actMonth == 13)
					{
						actMonth = 1;
						actYear++;
					}
					sprintf(szDateEnd, "%04d-%02d-%02d", actYear, actMonth, 1);
					sprintf(szDateEndPrev, "%04d-%02d-%02d", actYear - 1, actMonth, 1);
				}
				else if (!sactyear.empty())
				{
					sprintf(szDateStart, "%04d-%02d-%02d", actYear, 1, 1);
					sprintf(szDateStartPrev, "%04d-%02d-%02d", actYear - 1, 1, 1);
					actYear++;
					sprintf(szDateEnd, "%04d-%02d-%02d", actYear, 1, 1);
					sprintf(szDateEndPrev, "%04d-%02d-%02d", actYear - 1, 1, 1);
				}
				else
				{
					sprintf(szDateEnd, "%04d-%02d-%02d", tm1.tm_year + 1900, tm1.tm_mon + 1, tm1.tm_mday);
					sprintf(szDateEndPrev, "%04d-%02d-%02d", tm1.tm_year + 1900 - 1, tm1.tm_mon + 1, tm1.tm_mday);

					struct tm tm2;
					if (srange == "month")
					{
						// Subtract one month
						time_t monthbefore;
						getNoon(monthbefore, tm2, tm1.tm_year + 1900, tm1.tm_mon, tm1.tm_mday);
					}
					else
					{
						// Subtract one year
						time_t yearbefore;
						getNoon(yearbefore, tm2, tm1.tm_year + 1900 - 1, tm1.tm_mon + 1, tm1.tm_mday);
					}

					sprintf(szDateStart, "%04d-%02d-%02d", tm2.tm_year + 1900, tm2.tm_mon + 1, tm2.tm_mday);
					sprintf(szDateStartPrev, "%04d-%02d-%02d", tm2.tm_year + 1900 - 1, tm2.tm_mon + 1, tm2.tm_mday);
				}

				if (sensor == "temp")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					// Actual Year
					result = m_sql.safe_query("SELECT Temp_Min, Temp_Max, Chill_Min, Chill_Max,"
						" Humidity, Barometer, Temp_Avg, Date, SetPoint_Min,"
						" SetPoint_Max, SetPoint_Avg "
						"FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q'"
						" AND Date<='%q') ORDER BY Date ASC",
						dbasetable.c_str(), idx, szDateStart, szDateEnd);
					int ii = 0;
					if (!result.empty())
					{
						for (const auto& sd : result)
						{
							root["result"][ii]["d"] = sd[7].substr(0, 16);

							if ((dType == pTypeRego6XXTemp) || (dType == pTypeTEMP) || (dType == pTypeTEMP_HUM) || (dType == pTypeTEMP_HUM_BARO) ||
								(dType == pTypeTEMP_BARO) || (dType == pTypeWIND) || (dType == pTypeThermostat1) || (dType == pTypeRadiator1) ||
								((dType == pTypeRFXSensor) && (dSubType == sTypeRFXSensorTemp)) || ((dType == pTypeUV) && (dSubType == sTypeUV3)) ||
								((dType == pTypeGeneral) && (dSubType == sTypeSystemTemp)) || ((dType == pTypeThermostat) && (dSubType == sTypeThermSetpoint)) ||
								(dType == pTypeEvohomeZone) || (dType == pTypeEvohomeWater) || ((dType == pTypeGeneral) && (dSubType == sTypeBaro)))
							{
								bool bOK = true;
								if (dType == pTypeWIND)
								{
									bOK = ((dSubType != sTypeWINDNoTemp) && (dSubType != sTypeWINDNoTempNoChill));
								}
								if (bOK)
								{
									double te = ConvertTemperature(atof(sd[1].c_str()), tempsign);
									double tm = ConvertTemperature(atof(sd[0].c_str()), tempsign);
									double ta = ConvertTemperature(atof(sd[6].c_str()), tempsign);
									root["result"][ii]["te"] = te;
									root["result"][ii]["tm"] = tm;
									root["result"][ii]["ta"] = ta;
								}
							}
							if (((dType == pTypeWIND) && (dSubType == sTypeWIND4)) || ((dType == pTypeWIND) && (dSubType == sTypeWINDNoTemp)))
							{
								double ch = ConvertTemperature(atof(sd[3].c_str()), tempsign);
								double cm = ConvertTemperature(atof(sd[2].c_str()), tempsign);
								root["result"][ii]["ch"] = ch;
								root["result"][ii]["cm"] = cm;
							}
							if ((dType == pTypeHUM) || (dType == pTypeTEMP_HUM) || (dType == pTypeTEMP_HUM_BARO))
							{
								root["result"][ii]["hu"] = sd[4];
							}
							if ((dType == pTypeTEMP_HUM_BARO) || (dType == pTypeTEMP_BARO) || ((dType == pTypeGeneral) && (dSubType == sTypeBaro)))
							{
								if (dType == pTypeTEMP_HUM_BARO)
								{
									if (dSubType == sTypeTHBFloat)
									{
										sprintf(szTmp, "%.1f", atof(sd[5].c_str()) / 10.0F);
										root["result"][ii]["ba"] = szTmp;
									}
									else
										root["result"][ii]["ba"] = sd[5];
								}
								else if (dType == pTypeTEMP_BARO)
								{
									sprintf(szTmp, "%.1f", atof(sd[5].c_str()) / 10.0F);
									root["result"][ii]["ba"] = szTmp;
								}
								else if ((dType == pTypeGeneral) && (dSubType == sTypeBaro))
								{
									sprintf(szTmp, "%.1f", atof(sd[5].c_str()) / 10.0F);
									root["result"][ii]["ba"] = szTmp;
								}
							}
							if ((dType == pTypeEvohomeZone) || (dType == pTypeEvohomeWater))
							{
								double sm = ConvertTemperature(atof(sd[8].c_str()), tempsign);
								double sx = ConvertTemperature(atof(sd[9].c_str()), tempsign);
								double se = ConvertTemperature(atof(sd[10].c_str()), tempsign);
								root["result"][ii]["sm"] = sm;
								root["result"][ii]["se"] = se;
								root["result"][ii]["sx"] = sx;
							}
							ii++;
						}
					}
					// add today (have to calculate it)
					result = m_sql.safe_query("SELECT MIN(Temperature), MAX(Temperature),"
						" MIN(Chill), MAX(Chill), AVG(Humidity),"
						" AVG(Barometer), AVG(Temperature), MIN(SetPoint),"
						" MAX(SetPoint), AVG(SetPoint) "
						"FROM Temperature WHERE (DeviceRowID==%" PRIu64 ""
						" AND Date>='%q')",
						idx, szDateEnd);
					if (!result.empty())
					{
						std::vector<std::string> sd = result[0];

						root["result"][ii]["d"] = szDateEnd;
						if (((dType == pTypeRego6XXTemp) || (dType == pTypeTEMP) || (dType == pTypeTEMP_HUM) || (dType == pTypeTEMP_HUM_BARO) || (dType == pTypeTEMP_BARO) ||
							(dType == pTypeWIND) || (dType == pTypeThermostat1) || (dType == pTypeRadiator1)) ||
							((dType == pTypeUV) && (dSubType == sTypeUV3)) || ((dType == pTypeWIND) && (dSubType == sTypeWIND4)) || (dType == pTypeEvohomeZone) ||
							(dType == pTypeEvohomeWater))
						{
							double te = ConvertTemperature(atof(sd[1].c_str()), tempsign);
							double tm = ConvertTemperature(atof(sd[0].c_str()), tempsign);
							double ta = ConvertTemperature(atof(sd[6].c_str()), tempsign);

							root["result"][ii]["te"] = te;
							root["result"][ii]["tm"] = tm;
							root["result"][ii]["ta"] = ta;
						}
						if (((dType == pTypeWIND) && (dSubType == sTypeWIND4)) || ((dType == pTypeWIND) && (dSubType == sTypeWINDNoTemp)))
						{
							double ch = ConvertTemperature(atof(sd[3].c_str()), tempsign);
							double cm = ConvertTemperature(atof(sd[2].c_str()), tempsign);
							root["result"][ii]["ch"] = ch;
							root["result"][ii]["cm"] = cm;
						}
						if ((dType == pTypeHUM) || (dType == pTypeTEMP_HUM) || (dType == pTypeTEMP_HUM_BARO))
						{
							root["result"][ii]["hu"] = sd[4];
						}
						if ((dType == pTypeTEMP_HUM_BARO) || (dType == pTypeTEMP_BARO) || ((dType == pTypeGeneral) && (dSubType == sTypeBaro)))
						{
							if (dType == pTypeTEMP_HUM_BARO)
							{
								if (dSubType == sTypeTHBFloat)
								{
									sprintf(szTmp, "%.1f", atof(sd[5].c_str()) / 10.0F);
									root["result"][ii]["ba"] = szTmp;
								}
								else
									root["result"][ii]["ba"] = sd[5];
							}
							else if (dType == pTypeTEMP_BARO)
							{
								sprintf(szTmp, "%.1f", atof(sd[5].c_str()) / 10.0F);
								root["result"][ii]["ba"] = szTmp;
							}
							else if ((dType == pTypeGeneral) && (dSubType == sTypeBaro))
							{
								sprintf(szTmp, "%.1f", atof(sd[5].c_str()) / 10.0F);
								root["result"][ii]["ba"] = szTmp;
							}
						}
						if ((dType == pTypeEvohomeZone) || (dType == pTypeEvohomeWater))
						{
							double sx = ConvertTemperature(atof(sd[8].c_str()), tempsign);
							double sm = ConvertTemperature(atof(sd[7].c_str()), tempsign);
							double se = ConvertTemperature(atof(sd[9].c_str()), tempsign);
							root["result"][ii]["se"] = se;
							root["result"][ii]["sm"] = sm;
							root["result"][ii]["sx"] = sx;
						}
						ii++;
					}
					// Previous Year
					result = m_sql.safe_query("SELECT Temp_Min, Temp_Max, Chill_Min, Chill_Max,"
						" Humidity, Barometer, Temp_Avg, Date, SetPoint_Min,"
						" SetPoint_Max, SetPoint_Avg "
						"FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q'"
						" AND Date<='%q') ORDER BY Date ASC",
						dbasetable.c_str(), idx, szDateStartPrev, szDateEndPrev);
					if (!result.empty())
					{
						iPrev = 0;
						for (const auto& sd : result)
						{
							root["resultprev"][iPrev]["d"] = sd[7].substr(0, 16);

							if ((dType == pTypeRego6XXTemp) || (dType == pTypeTEMP) || (dType == pTypeTEMP_HUM) || (dType == pTypeTEMP_HUM_BARO) ||
								(dType == pTypeTEMP_BARO) || (dType == pTypeWIND) || (dType == pTypeThermostat1) || (dType == pTypeRadiator1) ||
								((dType == pTypeRFXSensor) && (dSubType == sTypeRFXSensorTemp)) || ((dType == pTypeUV) && (dSubType == sTypeUV3)) ||
								((dType == pTypeGeneral) && (dSubType == sTypeSystemTemp)) || ((dType == pTypeThermostat) && (dSubType == sTypeThermSetpoint)) ||
								(dType == pTypeEvohomeZone) || (dType == pTypeEvohomeWater))
							{
								bool bOK = true;
								if (dType == pTypeWIND)
								{
									bOK = ((dSubType == sTypeWIND4) || (dSubType == sTypeWINDNoTemp));
								}
								if (bOK)
								{
									double te = ConvertTemperature(atof(sd[1].c_str()), tempsign);
									double tm = ConvertTemperature(atof(sd[0].c_str()), tempsign);
									double ta = ConvertTemperature(atof(sd[6].c_str()), tempsign);
									root["resultprev"][iPrev]["te"] = te;
									root["resultprev"][iPrev]["tm"] = tm;
									root["resultprev"][iPrev]["ta"] = ta;
								}
							}
							if (((dType == pTypeWIND) && (dSubType == sTypeWIND4)) || ((dType == pTypeWIND) && (dSubType == sTypeWINDNoTemp)))
							{
								double ch = ConvertTemperature(atof(sd[3].c_str()), tempsign);
								double cm = ConvertTemperature(atof(sd[2].c_str()), tempsign);
								root["resultprev"][iPrev]["ch"] = ch;
								root["resultprev"][iPrev]["cm"] = cm;
							}
							if ((dType == pTypeHUM) || (dType == pTypeTEMP_HUM) || (dType == pTypeTEMP_HUM_BARO))
							{
								root["resultprev"][iPrev]["hu"] = sd[4];
							}
							if ((dType == pTypeTEMP_HUM_BARO) || (dType == pTypeTEMP_BARO) || ((dType == pTypeGeneral) && (dSubType == sTypeBaro)))
							{
								if (dType == pTypeTEMP_HUM_BARO)
								{
									if (dSubType == sTypeTHBFloat)
									{
										sprintf(szTmp, "%.1f", atof(sd[5].c_str()) / 10.0F);
										root["resultprev"][iPrev]["ba"] = szTmp;
									}
									else
										root["resultprev"][iPrev]["ba"] = sd[5];
								}
								else if (dType == pTypeTEMP_BARO)
								{
									sprintf(szTmp, "%.1f", atof(sd[5].c_str()) / 10.0F);
									root["resultprev"][iPrev]["ba"] = szTmp;
								}
								else if ((dType == pTypeGeneral) && (dSubType == sTypeBaro))
								{
									sprintf(szTmp, "%.1f", atof(sd[5].c_str()) / 10.0F);
									root["resultprev"][iPrev]["ba"] = szTmp;
								}
							}
							if ((dType == pTypeEvohomeZone) || (dType == pTypeEvohomeWater))
							{
								double sx = ConvertTemperature(atof(sd[8].c_str()), tempsign);
								double sm = ConvertTemperature(atof(sd[7].c_str()), tempsign);
								double se = ConvertTemperature(atof(sd[9].c_str()), tempsign);
								root["resultprev"][iPrev]["se"] = se;
								root["resultprev"][iPrev]["sm"] = sm;
								root["resultprev"][iPrev]["sx"] = sx;
							}
							iPrev++;
						}
					}
				}
				else if (sensor == "Percentage")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					result = m_sql.safe_query("SELECT Percentage_Min, Percentage_Max, Percentage_Avg, Date FROM %s WHERE (DeviceRowID==%" PRIu64
						" AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
						dbasetable.c_str(), idx, szDateStart, szDateEnd);
					int ii = 0;
					if (!result.empty())
					{
						for (const auto& sd : result)
						{
							root["result"][ii]["d"] = sd[3].substr(0, 16);
							root["result"][ii]["v_min"] = sd[0];
							root["result"][ii]["v_max"] = sd[1];
							root["result"][ii]["v_avg"] = sd[2];
							ii++;
						}
					}
					// add today (have to calculate it)
					result = m_sql.safe_query("SELECT MIN(Percentage), MAX(Percentage), AVG(Percentage) FROM Percentage WHERE (DeviceRowID=%" PRIu64 " AND Date>='%q')", idx,
						szDateEnd);
					if (!result.empty())
					{
						std::vector<std::string> sd = result[0];
						root["result"][ii]["d"] = szDateEnd;
						root["result"][ii]["v_min"] = sd[0];
						root["result"][ii]["v_max"] = sd[1];
						root["result"][ii]["v_avg"] = sd[2];
						ii++;
					}
				}
				else if (sensor == "fan")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					result = m_sql.safe_query("SELECT Speed_Min, Speed_Max, Date FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
						dbasetable.c_str(), idx, szDateStart, szDateEnd);
					int ii = 0;
					if (!result.empty())
					{
						for (const auto& sd : result)
						{
							root["result"][ii]["d"] = sd[2].substr(0, 16);
							root["result"][ii]["v_max"] = sd[1];
							root["result"][ii]["v_min"] = sd[0];
							ii++;
						}
					}
					// add today (have to calculate it)
					result = m_sql.safe_query("SELECT MIN(Speed), MAX(Speed) FROM Fan WHERE (DeviceRowID=%" PRIu64 " AND Date>='%q')", idx, szDateEnd);
					if (!result.empty())
					{
						std::vector<std::string> sd = result[0];
						root["result"][ii]["d"] = szDateEnd;
						root["result"][ii]["v_max"] = sd[1];
						root["result"][ii]["v_min"] = sd[0];
						ii++;
					}
				}
				else if (sensor == "uv")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					result = m_sql.safe_query("SELECT Level, Date FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC", dbasetable.c_str(),
						idx, szDateStart, szDateEnd);
					int ii = 0;
					if (!result.empty())
					{
						for (const auto& sd : result)
						{
							root["result"][ii]["d"] = sd[1].substr(0, 16);
							root["result"][ii]["uvi"] = sd[0];
							ii++;
						}
					}
					// add today (have to calculate it)
					result = m_sql.safe_query("SELECT MAX(Level) FROM UV WHERE (DeviceRowID=%" PRIu64 " AND Date>='%q')", idx, szDateEnd);
					if (!result.empty())
					{
						std::vector<std::string> sd = result[0];

						root["result"][ii]["d"] = szDateEnd;
						root["result"][ii]["uvi"] = sd[0];
						ii++;
					}
					// Previous Year
					result = m_sql.safe_query("SELECT Level, Date FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC", dbasetable.c_str(),
						idx, szDateStartPrev, szDateEndPrev);
					if (!result.empty())
					{
						iPrev = 0;
						for (const auto& sd : result)
						{
							root["resultprev"][iPrev]["d"] = sd[1].substr(0, 16);
							root["resultprev"][iPrev]["uvi"] = sd[0];
							iPrev++;
						}
					}
				}
				else if (sensor == "rain")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					result = m_sql.safe_query("SELECT Total, Rate, Date FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
						dbasetable.c_str(), idx, szDateStart, szDateEnd);
					int ii = 0;
					if (!result.empty())
					{
						for (const auto& sd : result)
						{
							root["result"][ii]["d"] = sd[2].substr(0, 16);
							double mmval = atof(sd[0].c_str());
							mmval *= AddjMulti;
							sprintf(szTmp, "%.1f", mmval);
							root["result"][ii]["mm"] = szTmp;
							ii++;
						}
					}
					// add today (have to calculate it)
					if (dSubType == sTypeRAINWU || dSubType == sTypeRAINByRate)
					{
						result = m_sql.safe_query("SELECT Total, Total, Rate FROM Rain WHERE (DeviceRowID=%" PRIu64 " AND Date>='%q') ORDER BY ROWID DESC LIMIT 1", idx,
							szDateEnd);
					}
					else
					{
						result = m_sql.safe_query("SELECT MIN(Total), MAX(Total), MAX(Rate) FROM Rain WHERE (DeviceRowID=%" PRIu64 " AND Date>='%q')", idx, szDateEnd);
					}
					if (!result.empty())
					{
						std::vector<std::string> sd = result[0];

						float total_min = static_cast<float>(atof(sd[0].c_str()));
						float total_max = static_cast<float>(atof(sd[1].c_str()));
						// int rate = atoi(sd[2].c_str());

						double total_real = 0;
						if (dSubType == sTypeRAINWU || dSubType == sTypeRAINByRate)
						{
							total_real = total_max;
						}
						else
						{
							total_real = total_max - total_min;
						}
						total_real *= AddjMulti;
						sprintf(szTmp, "%.1f", total_real);
						root["result"][ii]["d"] = szDateEnd;
						root["result"][ii]["mm"] = szTmp;
						ii++;
					}
					// Previous Year
					result = m_sql.safe_query("SELECT Total, Rate, Date FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
						dbasetable.c_str(), idx, szDateStartPrev, szDateEndPrev);
					if (!result.empty())
					{
						iPrev = 0;
						for (const auto& sd : result)
						{
							root["resultprev"][iPrev]["d"] = sd[2].substr(0, 16);
							double mmval = atof(sd[0].c_str());
							mmval *= AddjMulti;
							sprintf(szTmp, "%.1f", mmval);
							root["resultprev"][iPrev]["mm"] = szTmp;
							iPrev++;
						}
					}
				}
				else if (sensor == "counter")
				{
					root["status"] = "OK";
					root["title"] = sgroupby.empty() ? "Graph " + sensor + " " + srange : "Comparing " + sensor;
					root["ValueQuantity"] = options["ValueQuantity"];
					root["ValueUnits"] = options["ValueUnits"];
					root["Divider"] = divider;

					// int nValue = 0;
					std::string sValue; //Counter

					result = m_sql.safe_query("SELECT sValue FROM DeviceStatus WHERE (ID==%" PRIu64 ")", idx);
					if (!result.empty())
					{
						sValue = result[0][0];
					}

					std::function<std::string(std::string, std::string)> tableColumn = [](std::string table, std::string expr) {
						return (table.empty() ? "" : table + ".") + expr;
					};

					int ii = 0;
					iPrev = 0;
					if (dType == pTypeP1Power)
					{
						if (!sgroupby.empty()) {
							if (sensorarea.empty())
							{
								_log.Log(LOG_ERROR, "Parameter sensorarea missing with groupby '%s'", sgroupby.c_str());
								return;
							}
							std::function<std::string(const char*, char*, char*, char*, char*)> sensorareaExpr =
								[sensorarea, this](const char* expr, char* usageLow, char* usageNormal, char* deliveryLow, char* deliveryNormal) {
								if (sensorarea == "usage")
								{
									return std_format(expr, usageLow, usageNormal);
								}
								if (sensorarea == "delivery")
								{
									return std_format(expr, deliveryLow, deliveryNormal);
								}
								return std::string(expr);
							};
							std::function<std::string(std::string)> counterExpr = [sensorareaExpr](std::string expr) {
								return sensorareaExpr(expr.c_str(), "1", "3", "2", "4");
							};
							std::function<std::string(std::string)> valueExpr = [sensorareaExpr](std::string expr) {
								return sensorareaExpr(expr.c_str(), "1", "5", "2", "6");
							};
							GroupBy(
								root, dbasetable, idx, sgroupby,
								[counterExpr, tableColumn](std::string table) {
									return counterExpr(tableColumn(table, "Counter%s") + "+" + tableColumn(table, "Counter%s"));
								},
								[valueExpr, tableColumn](std::string table) { return valueExpr(tableColumn(table, "Value%s") + "+" + tableColumn(table, "Value%s")); },
									[divider, this](double sum) {
									if (sum == 0)
									{
										return std::string("0");
									}
									return std_format("%.3f", sum / divider);
								});
							ii = root["result"].size();
						}
						else
						{
							// Actual Year
							result = m_sql.safe_query("SELECT Value1,Value2,Value5,Value6, Date,"
								" Counter1, Counter2, Counter3, Counter4 "
								"FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q'"
								" AND Date<='%q') ORDER BY Date ASC",
								dbasetable.c_str(), idx, szDateStart, szDateEnd);
							if (!result.empty())
							{
								bool bHaveDeliverd = false;
								for (const auto& sd : result)
								{
									root["result"][ii]["d"] = sd[4].substr(0, 16);

									double counter_1 = std::stod(sd[5]);
									double counter_2 = std::stod(sd[6]);
									double counter_3 = std::stod(sd[7]);
									double counter_4 = std::stod(sd[8]);

									float fUsage_1 = std::stof(sd[0]);
									float fUsage_2 = std::stof(sd[2]);
									float fDeliv_1 = std::stof(sd[1]);
									float fDeliv_2 = std::stof(sd[3]);

									fDeliv_1 = (fDeliv_1 < 10) ? 0 : fDeliv_1;
									fDeliv_2 = (fDeliv_2 < 10) ? 0 : fDeliv_2;

									if ((fDeliv_1 != 0) || (fDeliv_2 != 0))
									{
										bHaveDeliverd = true;
									}
									sprintf(szTmp, "%.3f", fUsage_1 / divider);
									root["result"][ii]["v"] = szTmp;
									sprintf(szTmp, "%.3f", fUsage_2 / divider);
									root["result"][ii]["v2"] = szTmp;
									sprintf(szTmp, "%.3f", fDeliv_1 / divider);
									root["result"][ii]["r1"] = szTmp;
									sprintf(szTmp, "%.3f", fDeliv_2 / divider);
									root["result"][ii]["r2"] = szTmp;

									if (counter_1 != 0)
									{
										sprintf(szTmp, "%.3f", (counter_1 - fUsage_1) / divider);
									}
									else
									{
										strcpy(szTmp, "0");
									}
									root["result"][ii]["c1"] = szTmp;

									if (counter_2 != 0)
									{
										sprintf(szTmp, "%.3f", (counter_2 - fDeliv_1) / divider);
									}
									else
									{
										strcpy(szTmp, "0");
									}
									root["result"][ii]["c2"] = szTmp;

									if (counter_3 != 0)
									{
										sprintf(szTmp, "%.3f", (counter_3 - fUsage_2) / divider);
									}
									else
									{
										strcpy(szTmp, "0");
									}
									root["result"][ii]["c3"] = szTmp;

									if (counter_4 != 0)
									{
										sprintf(szTmp, "%.3f", (counter_4 - fDeliv_2) / divider);
									}
									else
									{
										strcpy(szTmp, "0");
									}
									root["result"][ii]["c4"] = szTmp;

									ii++;
								}
								if (bHaveDeliverd)
								{
									root["delivered"] = true;
								}
							}
							// Previous Year
							result = m_sql.safe_query("SELECT Value1,Value2,Value5,Value6, Date "
								"FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
								dbasetable.c_str(), idx, szDateStartPrev, szDateEndPrev);
							if (!result.empty())
							{
								bool bHaveDeliverd = false;
								iPrev = 0;
								for (const auto& sd : result)
								{
									root["resultprev"][iPrev]["d"] = sd[4].substr(0, 16);

									float fUsage_1 = std::stof(sd[0]);
									float fUsage_2 = std::stof(sd[2]);
									float fDeliv_1 = std::stof(sd[1]);
									float fDeliv_2 = std::stof(sd[3]);

									if ((fDeliv_1 != 0) || (fDeliv_2 != 0))
									{
										bHaveDeliverd = true;
									}
									sprintf(szTmp, "%.3f", fUsage_1 / divider);
									root["resultprev"][iPrev]["v"] = szTmp;
									sprintf(szTmp, "%.3f", fUsage_2 / divider);
									root["resultprev"][iPrev]["v2"] = szTmp;
									sprintf(szTmp, "%.3f", fDeliv_1 / divider);
									root["resultprev"][iPrev]["r1"] = szTmp;
									sprintf(szTmp, "%.3f", fDeliv_2 / divider);
									root["resultprev"][iPrev]["r2"] = szTmp;
									iPrev++;
								}
								if (bHaveDeliverd)
								{
									root["delivered"] = true;
								}
							}
						}
					}
					else if (dType == pTypeAirQuality)
					{ // month/year
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;

						result = m_sql.safe_query("SELECT Value1,Value2,Value3,Date FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
							dbasetable.c_str(), idx, szDateStart, szDateEnd);
						if (!result.empty())
						{
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[3].substr(0, 16);
								root["result"][ii]["co2_min"] = sd[0];
								root["result"][ii]["co2_max"] = sd[1];
								root["result"][ii]["co2_avg"] = sd[2];
								ii++;
							}
						}
						result = m_sql.safe_query("SELECT Value2,Date FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
							dbasetable.c_str(), idx, szDateStartPrev, szDateEndPrev);
						if (!result.empty())
						{
							iPrev = 0;
							for (const auto& sd : result)
							{
								root["resultprev"][iPrev]["d"] = sd[1].substr(0, 16);
								root["resultprev"][iPrev]["co2_max"] = sd[0];
								iPrev++;
							}
						}
					}
					else if (((dType == pTypeGeneral) && ((dSubType == sTypeSoilMoisture) || (dSubType == sTypeLeafWetness))) ||
						((dType == pTypeRFXSensor) && ((dSubType == sTypeRFXSensorAD) || (dSubType == sTypeRFXSensorVolt))))
					{ // month/year
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;

						result = m_sql.safe_query("SELECT Value1,Value2, Date FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
							dbasetable.c_str(), idx, szDateStart, szDateEnd);
						if (!result.empty())
						{
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[2].substr(0, 16);
								root["result"][ii]["v_min"] = sd[0];
								root["result"][ii]["v_max"] = sd[1];
								ii++;
							}
						}
					}
					else if (((dType == pTypeGeneral) && (dSubType == sTypeVisibility)) || ((dType == pTypeGeneral) && (dSubType == sTypeDistance)) ||
						((dType == pTypeGeneral) && (dSubType == sTypeSolarRadiation)) || ((dType == pTypeGeneral) && (dSubType == sTypeVoltage)) ||
						((dType == pTypeGeneral) && (dSubType == sTypeCurrent)) || ((dType == pTypeGeneral) && (dSubType == sTypePressure)) ||
						((dType == pTypeGeneral) && (dSubType == sTypeSoundLevel)))
					{ // month/year
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;

						float vdiv = 10.0F;
						if (((dType == pTypeGeneral) && (dSubType == sTypeVoltage)) || ((dType == pTypeGeneral) && (dSubType == sTypeCurrent)))
						{
							vdiv = 1000.0F;
						}

						result = m_sql.safe_query("SELECT Value1,Value2,Value3,Date FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
							dbasetable.c_str(), idx, szDateStart, szDateEnd);
						if (!result.empty())
						{
							for (const auto& sd : result)
							{
								float fValue1 = float(atof(sd[0].c_str())) / vdiv;
								float fValue2 = float(atof(sd[1].c_str())) / vdiv;
								float fValue3 = float(atof(sd[2].c_str())) / vdiv;
								root["result"][ii]["d"] = sd[3].substr(0, 16);

								if (metertype == 1)
								{
									if ((dType == pTypeGeneral) && (dSubType == sTypeDistance))
									{
										// Inches
										fValue1 *= 0.3937007874015748F;
										fValue2 *= 0.3937007874015748F;
									}
									else
									{
										// Miles
										fValue1 *= 0.6214F;
										fValue2 *= 0.6214F;
									}
								}
								if (((dType == pTypeGeneral) && (dSubType == sTypeVoltage)) || ((dType == pTypeGeneral) && (dSubType == sTypeCurrent)))
								{
									sprintf(szTmp, "%.3f", fValue1);
									root["result"][ii]["v_min"] = szTmp;
									sprintf(szTmp, "%.3f", fValue2);
									root["result"][ii]["v_max"] = szTmp;
									if (fValue3 != 0)
									{
										sprintf(szTmp, "%.3f", fValue3);
										root["result"][ii]["v_avg"] = szTmp;
									}
								}
								else
								{
									sprintf(szTmp, "%.1f", fValue1);
									root["result"][ii]["v_min"] = szTmp;
									sprintf(szTmp, "%.1f", fValue2);
									root["result"][ii]["v_max"] = szTmp;
									if (fValue3 != 0)
									{
										sprintf(szTmp, "%.1f", fValue3);
										root["result"][ii]["v_avg"] = szTmp;
									}
								}
								ii++;
							}
						}
					}
					else if (dType == pTypeLux)
					{ // month/year
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;

						result = m_sql.safe_query("SELECT Value1,Value2,Value3, Date FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
							dbasetable.c_str(), idx, szDateStart, szDateEnd);
						if (!result.empty())
						{
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[3].substr(0, 16);
								root["result"][ii]["lux_min"] = sd[0];
								root["result"][ii]["lux_max"] = sd[1];
								root["result"][ii]["lux_avg"] = sd[2];
								ii++;
							}
						}
					}
					else if (dType == pTypeWEIGHT)
					{ // month/year
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;

						result = m_sql.safe_query("SELECT Value1,Value2, Date FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
							dbasetable.c_str(), idx, szDateStart, szDateEnd);
						if (!result.empty())
						{
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[2].substr(0, 16);
								sprintf(szTmp, "%.1f", m_sql.m_weightscale * atof(sd[0].c_str()) / 10.0F);
								root["result"][ii]["v_min"] = szTmp;
								sprintf(szTmp, "%.1f", m_sql.m_weightscale * atof(sd[1].c_str()) / 10.0F);
								root["result"][ii]["v_max"] = szTmp;
								ii++;
							}
						}
					}
					else if (dType == pTypeUsage)
					{ // month/year
						root["status"] = "OK";
						root["title"] = "Graph " + sensor + " " + srange;

						result = m_sql.safe_query("SELECT Value1,Value2, Date FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
							dbasetable.c_str(), idx, szDateStart, szDateEnd);
						if (!result.empty())
						{
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[2].substr(0, 16);
								root["result"][ii]["u_min"] = atof(sd[0].c_str()) / 10.0F;
								root["result"][ii]["u_max"] = atof(sd[1].c_str()) / 10.0F;
								ii++;
							}
						}
					}
					else if (dType == pTypeCURRENT)
					{
						result = m_sql.safe_query("SELECT Value1,Value2,Value3,Value4,Value5,Value6, Date FROM %s WHERE (DeviceRowID==%" PRIu64
							" AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
							dbasetable.c_str(), idx, szDateStart, szDateEnd);
						if (!result.empty())
						{
							// CM113
							int displaytype = 0;
							int voltage = 230;
							m_sql.GetPreferencesVar("CM113DisplayType", displaytype);
							m_sql.GetPreferencesVar("ElectricVoltage", voltage);

							root["displaytype"] = displaytype;

							bool bHaveL1 = false;
							bool bHaveL2 = false;
							bool bHaveL3 = false;
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[6].substr(0, 16);

								float fval1 = static_cast<float>(atof(sd[0].c_str()) / 10.0F);
								float fval2 = static_cast<float>(atof(sd[1].c_str()) / 10.0F);
								float fval3 = static_cast<float>(atof(sd[2].c_str()) / 10.0F);
								float fval4 = static_cast<float>(atof(sd[3].c_str()) / 10.0F);
								float fval5 = static_cast<float>(atof(sd[4].c_str()) / 10.0F);
								float fval6 = static_cast<float>(atof(sd[5].c_str()) / 10.0F);

								if ((fval1 != 0) || (fval2 != 0))
									bHaveL1 = true;
								if ((fval3 != 0) || (fval4 != 0))
									bHaveL2 = true;
								if ((fval5 != 0) || (fval6 != 0))
									bHaveL3 = true;

								if (displaytype == 0)
								{
									sprintf(szTmp, "%.1f", fval1);
									root["result"][ii]["v1"] = szTmp;
									sprintf(szTmp, "%.1f", fval2);
									root["result"][ii]["v2"] = szTmp;
									sprintf(szTmp, "%.1f", fval3);
									root["result"][ii]["v3"] = szTmp;
									sprintf(szTmp, "%.1f", fval4);
									root["result"][ii]["v4"] = szTmp;
									sprintf(szTmp, "%.1f", fval5);
									root["result"][ii]["v5"] = szTmp;
									sprintf(szTmp, "%.1f", fval6);
									root["result"][ii]["v6"] = szTmp;
								}
								else
								{
									sprintf(szTmp, "%d", int(fval1 * voltage));
									root["result"][ii]["v1"] = szTmp;
									sprintf(szTmp, "%d", int(fval2 * voltage));
									root["result"][ii]["v2"] = szTmp;
									sprintf(szTmp, "%d", int(fval3 * voltage));
									root["result"][ii]["v3"] = szTmp;
									sprintf(szTmp, "%d", int(fval4 * voltage));
									root["result"][ii]["v4"] = szTmp;
									sprintf(szTmp, "%d", int(fval5 * voltage));
									root["result"][ii]["v5"] = szTmp;
									sprintf(szTmp, "%d", int(fval6 * voltage));
									root["result"][ii]["v6"] = szTmp;
								}

								ii++;
							}
							if ((!bHaveL1) && (!bHaveL2) && (!bHaveL3))
							{
								root["haveL1"] = true; // show at least something
							}
							else
							{
								if (bHaveL1)
									root["haveL1"] = true;
								if (bHaveL2)
									root["haveL2"] = true;
								if (bHaveL3)
									root["haveL3"] = true;
							}
						}
					}
					else if (dType == pTypeCURRENTENERGY)
					{
						result = m_sql.safe_query("SELECT Value1,Value2,Value3,Value4,Value5,Value6, Date FROM %s WHERE (DeviceRowID==%" PRIu64
							" AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
							dbasetable.c_str(), idx, szDateStart, szDateEnd);
						if (!result.empty())
						{
							// CM180i
							int displaytype = 0;
							int voltage = 230;
							m_sql.GetPreferencesVar("CM113DisplayType", displaytype);
							m_sql.GetPreferencesVar("ElectricVoltage", voltage);

							root["displaytype"] = displaytype;

							bool bHaveL1 = false;
							bool bHaveL2 = false;
							bool bHaveL3 = false;
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[6].substr(0, 16);

								float fval1 = static_cast<float>(atof(sd[0].c_str()) / 10.0F);
								float fval2 = static_cast<float>(atof(sd[1].c_str()) / 10.0F);
								float fval3 = static_cast<float>(atof(sd[2].c_str()) / 10.0F);
								float fval4 = static_cast<float>(atof(sd[3].c_str()) / 10.0F);
								float fval5 = static_cast<float>(atof(sd[4].c_str()) / 10.0F);
								float fval6 = static_cast<float>(atof(sd[5].c_str()) / 10.0F);

								if ((fval1 != 0) || (fval2 != 0))
									bHaveL1 = true;
								if ((fval3 != 0) || (fval4 != 0))
									bHaveL2 = true;
								if ((fval5 != 0) || (fval6 != 0))
									bHaveL3 = true;

								if (displaytype == 0)
								{
									sprintf(szTmp, "%.1f", fval1);
									root["result"][ii]["v1"] = szTmp;
									sprintf(szTmp, "%.1f", fval2);
									root["result"][ii]["v2"] = szTmp;
									sprintf(szTmp, "%.1f", fval3);
									root["result"][ii]["v3"] = szTmp;
									sprintf(szTmp, "%.1f", fval4);
									root["result"][ii]["v4"] = szTmp;
									sprintf(szTmp, "%.1f", fval5);
									root["result"][ii]["v5"] = szTmp;
									sprintf(szTmp, "%.1f", fval6);
									root["result"][ii]["v6"] = szTmp;
								}
								else
								{
									sprintf(szTmp, "%d", int(fval1 * voltage));
									root["result"][ii]["v1"] = szTmp;
									sprintf(szTmp, "%d", int(fval2 * voltage));
									root["result"][ii]["v2"] = szTmp;
									sprintf(szTmp, "%d", int(fval3 * voltage));
									root["result"][ii]["v3"] = szTmp;
									sprintf(szTmp, "%d", int(fval4 * voltage));
									root["result"][ii]["v4"] = szTmp;
									sprintf(szTmp, "%d", int(fval5 * voltage));
									root["result"][ii]["v5"] = szTmp;
									sprintf(szTmp, "%d", int(fval6 * voltage));
									root["result"][ii]["v6"] = szTmp;
								}

								ii++;
							}
							if ((!bHaveL1) && (!bHaveL2) && (!bHaveL3))
							{
								root["haveL1"] = true; // show at least something
							}
							else
							{
								if (bHaveL1)
									root["haveL1"] = true;
								if (bHaveL2)
									root["haveL2"] = true;
								if (bHaveL3)
									root["haveL3"] = true;
							}
						}
					}
					else
					{
						if (dType == pTypeP1Gas)
						{
							// Add last counter value
							sprintf(szTmp, "%.3f", atof(sValue.c_str()) / 1000.0);
							root["counter"] = szTmp;
						}
						else if (dType == pTypeENERGY)
						{
							size_t spos = sValue.find(';');
							if (spos != std::string::npos)
							{
								float fvalue = static_cast<float>(atof(sValue.substr(spos + 1).c_str()));
								sprintf(szTmp, "%.3f", fvalue / (divider / 100.0F));
								root["counter"] = szTmp;
							}
						}
						else if ((dType == pTypeGeneral) && (dSubType == sTypeKwh))
						{
							size_t spos = sValue.find(';');
							if (spos != std::string::npos)
							{
								float fvalue = static_cast<float>(atof(sValue.substr(spos + 1).c_str()));
								sprintf(szTmp, "%.3f", fvalue / divider);
								root["counter"] = szTmp;
							}
						}
						else if (dType == pTypeRFXMeter)
						{
							// Add last counter value
							double fvalue = atof(sValue.c_str());
							switch (metertype)
							{
							case MTYPE_ENERGY:
							case MTYPE_ENERGY_GENERATED:
								sprintf(szTmp, "%.3f", meteroffset + (fvalue / divider));
								break;
							case MTYPE_GAS:
								sprintf(szTmp, "%.2f", meteroffset + (fvalue / divider));
								break;
							case MTYPE_WATER:
								sprintf(szTmp, "%.3f", meteroffset + (fvalue / divider));
								break;
							case MTYPE_COUNTER:
								sprintf(szTmp, "%.10g", meteroffset + (fvalue / divider));
								break;
							default:
								strcpy(szTmp, "");
								break;
							}
							root["counter"] = szTmp;
						}
						else if (dType == pTypeYouLess)
						{
							std::vector<std::string> results;
							StringSplit(sValue, ";", results);
							if (results.size() == 2)
							{
								// Add last counter value
								double fvalue = atof(results[0].c_str());
								switch (metertype)
								{
								case MTYPE_ENERGY:
								case MTYPE_ENERGY_GENERATED:
									sprintf(szTmp, "%.3f", fvalue / divider);
									break;
								case MTYPE_GAS:
									sprintf(szTmp, "%.2f", fvalue / divider);
									break;
								case MTYPE_WATER:
									sprintf(szTmp, "%.3f", fvalue / divider);
									break;
								case MTYPE_COUNTER:
									sprintf(szTmp, "%.10g", fvalue / divider);
									break;
								default:
									strcpy(szTmp, "");
									break;
								}
								root["counter"] = szTmp;
							}
						}
						else if ((dType == pTypeGeneral) && (dSubType == sTypeCounterIncremental))
						{
							double dvalue = static_cast<double>(atof(sValue.c_str()));

							switch (metertype)
							{
							case MTYPE_ENERGY:
							case MTYPE_ENERGY_GENERATED:
								sprintf(szTmp, "%.3f", meteroffset + (dvalue / divider));
								break;
							case MTYPE_GAS:
								sprintf(szTmp, "%.3f", meteroffset + (dvalue / divider));
								break;
							case MTYPE_WATER:
								sprintf(szTmp, "%.3f", meteroffset + (dvalue / divider));
								break;
							case MTYPE_COUNTER:
							default:
								sprintf(szTmp, "%.10g", meteroffset + (dvalue / divider));
								break;
							}
							root["counter"] = szTmp;
						}
						else if (!bIsManagedCounter)
						{
							double dvalue = static_cast<double>(atof(sValue.c_str()));
							sprintf(szTmp, "%.10g", meteroffset + (dvalue / divider));
							root["counter"] = szTmp;
						}
						else
						{
							root["counter"] = "0";
						}
						if (!sgroupby.empty())
						{
							GroupBy(
								root, dbasetable, idx, sgroupby, [tableColumn](std::string table) { return tableColumn(table, "Counter"); },
								[tableColumn](std::string table) { return tableColumn(table, "Value"); },
								[metertype, AddjValue, divider, this](double sum) {
									if (sum == 0)
									{
										return std::string("0");
									}
									switch (metertype)
									{
									case MTYPE_ENERGY:
									case MTYPE_ENERGY_GENERATED:
										return std_format("%.3f", sum / divider);
									case MTYPE_GAS:
										return std_format("%.2f", sum / divider);
									case MTYPE_WATER:
										return std_format("%.3f", sum / divider);
									case MTYPE_COUNTER:
										return std_format("%.10g", sum / divider);
									}
									return std::string("");
								});
							ii = root["result"].size();
						}
						else
						{
							// Actual Year
							result =
								m_sql.safe_query("SELECT Value, Date, Counter FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
									dbasetable.c_str(), idx, szDateStart, szDateEnd);
							if (!result.empty())
							{
								for (const auto& sd : result)
								{
									root["result"][ii]["d"] = sd[1].substr(0, 16);

									std::string szValue = sd[0];

									double fcounter = atof(sd[2].c_str());

									switch (metertype)
									{
									case MTYPE_ENERGY:
									case MTYPE_ENERGY_GENERATED:
										sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
										root["result"][ii]["v"] = szTmp;
										if (fcounter != 0)
											sprintf(szTmp, "%.3f", meteroffset + ((fcounter - atof(szValue.c_str())) / divider));
										else
											strcpy(szTmp, "0");
										root["result"][ii]["c"] = szTmp;
										break;
									case MTYPE_GAS:
										sprintf(szTmp, "%.2f", atof(szValue.c_str()) / divider);
										root["result"][ii]["v"] = szTmp;
										if (fcounter != 0)
											sprintf(szTmp, "%.2f", meteroffset + ((fcounter - atof(szValue.c_str())) / divider));
										else
											strcpy(szTmp, "0");
										root["result"][ii]["c"] = szTmp;
										break;
									case MTYPE_WATER:
										sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
										root["result"][ii]["v"] = szTmp;
										if (fcounter != 0)
											sprintf(szTmp, "%.3f", meteroffset + ((fcounter - atof(szValue.c_str())) / divider));
										else
											strcpy(szTmp, "0");
										root["result"][ii]["c"] = szTmp;
										break;
									case MTYPE_COUNTER:
										sprintf(szTmp, "%.10g", atof(szValue.c_str()) / divider);
										root["result"][ii]["v"] = szTmp;
										if (fcounter != 0)
											sprintf(szTmp, "%.10g", meteroffset + ((fcounter - atof(szValue.c_str())) / divider));
										else
											strcpy(szTmp, "0");
										root["result"][ii]["c"] = szTmp;
										break;
									}
									ii++;
								}
							}
							// Past Year
							result =
								m_sql.safe_query("SELECT Value, Date, Counter FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
									dbasetable.c_str(), idx, szDateStartPrev, szDateEndPrev);
							if (!result.empty())
							{
								iPrev = 0;
								for (const auto& sd : result)
								{
									root["resultprev"][iPrev]["d"] = sd[1].substr(0, 16);

									std::string szValue = sd[0];
									switch (metertype)
									{
									case MTYPE_ENERGY:
									case MTYPE_ENERGY_GENERATED:
										sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
										root["resultprev"][iPrev]["v"] = szTmp;
										break;
									case MTYPE_GAS:
										sprintf(szTmp, "%.2f", atof(szValue.c_str()) / divider);
										root["resultprev"][iPrev]["v"] = szTmp;
										break;
									case MTYPE_WATER:
										sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
										root["resultprev"][iPrev]["v"] = szTmp;
										break;
									case MTYPE_COUNTER:
										sprintf(szTmp, "%.10g", atof(szValue.c_str()) / divider);
										root["resultprev"][iPrev]["v"] = szTmp;
										break;
									}
									iPrev++;
								}
							}
						}
					}
					// add today (have to calculate it)

					if ((!sactmonth.empty()) || (!sactyear.empty()))
					{
						struct tm loctime;
						time_t now = mytime(nullptr);
						localtime_r(&now, &loctime);
						if ((!sactmonth.empty()) && (!sactyear.empty()))
						{
							bool bIsThisMonth = (atoi(sactyear.c_str()) == loctime.tm_year + 1900) && (atoi(sactmonth.c_str()) == loctime.tm_mon + 1);
							if (bIsThisMonth)
							{
								sprintf(szDateEnd, "%04d-%02d-%02d", loctime.tm_year + 1900, loctime.tm_mon + 1, loctime.tm_mday);
							}
						}
						else if (!sactyear.empty())
						{
							bool bIsThisYear = (atoi(sactyear.c_str()) == loctime.tm_year + 1900);
							if (bIsThisYear)
							{
								sprintf(szDateEnd, "%04d-%02d-%02d", loctime.tm_year + 1900, loctime.tm_mon + 1, loctime.tm_mday);
							}
						}
					}

					if (dType == pTypeP1Power)
					{
						result = m_sql.safe_query("SELECT "
							" MIN(Value1) as levering_laag_min,"
							" MAX(Value1) as levering_laag_max,"
							" MIN(Value2) as teruglevering_laag_min,"
							" MAX(Value2) as teruglevering_laag_max,"
							" MIN(Value5) as levering_normaal_min,"
							" MAX(Value5) as levering_normaal_max,"
							" MIN(Value6) as teruglevering_normaal_min,"
							" MAX(Value6) as teruglevering_normaal_max"
							" FROM MultiMeter WHERE (DeviceRowID=%" PRIu64 ""
							" AND Date>='%q')",
							idx, szDateEnd);
						bool bHaveDeliverd = false;
						if (!result.empty())
						{
							std::vector<std::string> sd = result[0];
							uint64_t total_min_usage_1 = std::stoull(sd[0]);
							uint64_t total_max_usage_1 = std::stoull(sd[1]);
							uint64_t total_min_usage_2 = std::stoull(sd[4]);
							uint64_t total_max_usage_2 = std::stoull(sd[5]);
							uint64_t total_real_usage_1, total_real_usage_2;
							uint64_t total_min_deliv_1 = std::stoull(sd[2]);
							uint64_t total_max_deliv_1 = std::stoull(sd[3]);
							uint64_t total_min_deliv_2 = std::stoull(sd[6]);
							uint64_t total_max_deliv_2 = std::stoull(sd[7]);
							uint64_t total_real_deliv_1, total_real_deliv_2;

							total_real_usage_1 = total_max_usage_1 - total_min_usage_1;
							total_real_usage_2 = total_max_usage_2 - total_min_usage_2;

							total_real_deliv_1 = total_max_deliv_1 - total_min_deliv_1;
							total_real_deliv_2 = total_max_deliv_2 - total_min_deliv_2;

							if (total_max_deliv_1 != 0 || total_max_deliv_2 != 0)
								bHaveDeliverd = true;

							if (!sgroupby.empty())
							{
								const double todayValue = (sensorarea == "usage" ? (total_real_usage_1 + total_real_usage_2)
									: sensorarea == "delivery" ? (total_real_deliv_1 + total_real_deliv_2)
									: 0) /
									divider;
								AddTodayValueToResult(root, sgroupby, std::string(szDateEnd), todayValue, "%.3f");
							}
							else
							{
								root["result"][ii]["d"] = szDateEnd;

								sprintf(szTmp, "%.3f", (float)(total_real_usage_1 / divider));
								root["result"][ii]["v"] = szTmp;
								sprintf(szTmp, "%.3f", (float)(total_real_usage_2 / divider));
								root["result"][ii]["v2"] = szTmp;

								sprintf(szTmp, "%.3f", (float)(total_real_deliv_1 / divider));
								root["result"][ii]["r1"] = szTmp;
								sprintf(szTmp, "%.3f", (float)(total_real_deliv_2 / divider));
								root["result"][ii]["r2"] = szTmp;

								sprintf(szTmp, "%.3f", (float)(total_min_usage_1 / divider));
								root["result"][ii]["c1"] = szTmp;
								sprintf(szTmp, "%.3f", (float)(total_min_usage_2 / divider));
								root["result"][ii]["c3"] = szTmp;

								if (total_max_deliv_2 != 0)
								{
									sprintf(szTmp, "%.3f", (float)(total_min_deliv_1 / divider));
									root["result"][ii]["c2"] = szTmp;
									sprintf(szTmp, "%.3f", (float)(total_min_deliv_2 / divider));
									root["result"][ii]["c4"] = szTmp;
								}
								else
								{
									strcpy(szTmp, "0");
									root["result"][ii]["c2"] = szTmp;
									root["result"][ii]["c4"] = szTmp;
								}

								ii++;
							}
						}
						if (bHaveDeliverd)
						{
							root["delivered"] = true;
						}
					}
					else if (dType == pTypeAirQuality)
					{
						result = m_sql.safe_query("SELECT MIN(Value), MAX(Value), AVG(Value) FROM Meter WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q')", idx, szDateEnd);
						if (!result.empty())
						{
							root["result"][ii]["d"] = szDateEnd;
							root["result"][ii]["co2_min"] = result[0][0];
							root["result"][ii]["co2_max"] = result[0][1];
							root["result"][ii]["co2_avg"] = result[0][2];
							ii++;
						}
					}
					else if (((dType == pTypeGeneral) && ((dSubType == sTypeSoilMoisture) || (dSubType == sTypeLeafWetness))) ||
						((dType == pTypeRFXSensor) && ((dSubType == sTypeRFXSensorAD) || (dSubType == sTypeRFXSensorVolt))))
					{
						result = m_sql.safe_query("SELECT MIN(Value), MAX(Value) FROM Meter WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q')", idx, szDateEnd);
						if (!result.empty())
						{
							root["result"][ii]["d"] = szDateEnd;
							root["result"][ii]["v_min"] = result[0][0];
							root["result"][ii]["v_max"] = result[0][1];
							ii++;
						}
					}
					else if (((dType == pTypeGeneral) && (dSubType == sTypeVisibility)) || ((dType == pTypeGeneral) && (dSubType == sTypeDistance)) ||
						((dType == pTypeGeneral) && (dSubType == sTypeSolarRadiation)) || ((dType == pTypeGeneral) && (dSubType == sTypeVoltage)) ||
						((dType == pTypeGeneral) && (dSubType == sTypeCurrent)) || ((dType == pTypeGeneral) && (dSubType == sTypePressure)) ||
						((dType == pTypeGeneral) && (dSubType == sTypeSoundLevel)))
					{
						float vdiv = 10.0F;
						if (((dType == pTypeGeneral) && (dSubType == sTypeVoltage)) || ((dType == pTypeGeneral) && (dSubType == sTypeCurrent)))
						{
							vdiv = 1000.0F;
						}

						result = m_sql.safe_query("SELECT MIN(Value), MAX(Value) FROM Meter WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q')", idx, szDateEnd);
						if (!result.empty())
						{
							root["result"][ii]["d"] = szDateEnd;
							float fValue1 = float(atof(result[0][0].c_str())) / vdiv;
							float fValue2 = float(atof(result[0][1].c_str())) / vdiv;
							if (metertype == 1)
							{
								if ((dType == pTypeGeneral) && (dSubType == sTypeDistance))
								{
									// Inches
									fValue1 *= 0.3937007874015748F;
									fValue2 *= 0.3937007874015748F;
								}
								else
								{
									// Miles
									fValue1 *= 0.6214F;
									fValue2 *= 0.6214F;
								}
							}

							if ((dType == pTypeGeneral) && (dSubType == sTypeVoltage))
								sprintf(szTmp, "%.3f", fValue1);
							else if ((dType == pTypeGeneral) && (dSubType == sTypeCurrent))
								sprintf(szTmp, "%.3f", fValue1);
							else
								sprintf(szTmp, "%.1f", fValue1);
							root["result"][ii]["v_min"] = szTmp;
							if ((dType == pTypeGeneral) && (dSubType == sTypeVoltage))
								sprintf(szTmp, "%.3f", fValue2);
							else if ((dType == pTypeGeneral) && (dSubType == sTypeCurrent))
								sprintf(szTmp, "%.3f", fValue2);
							else
								sprintf(szTmp, "%.1f", fValue2);
							root["result"][ii]["v_max"] = szTmp;
							ii++;
						}
					}
					else if (dType == pTypeLux)
					{
						result = m_sql.safe_query("SELECT MIN(Value), MAX(Value), AVG(Value) FROM Meter WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q')", idx, szDateEnd);
						if (!result.empty())
						{
							root["result"][ii]["d"] = szDateEnd;
							root["result"][ii]["lux_min"] = result[0][0];
							root["result"][ii]["lux_max"] = result[0][1];
							root["result"][ii]["lux_avg"] = result[0][2];
							ii++;
						}
					}
					else if (dType == pTypeWEIGHT)
					{
						result = m_sql.safe_query("SELECT MIN(Value), MAX(Value) FROM Meter WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q')", idx, szDateEnd);
						if (!result.empty())
						{
							root["result"][ii]["d"] = szDateEnd;
							sprintf(szTmp, "%.1f", m_sql.m_weightscale * atof(result[0][0].c_str()) / 10.0F);
							root["result"][ii]["v_min"] = szTmp;
							sprintf(szTmp, "%.1f", m_sql.m_weightscale * atof(result[0][1].c_str()) / 10.0F);
							root["result"][ii]["v_max"] = szTmp;
							ii++;
						}
					}
					else if (dType == pTypeUsage)
					{
						result = m_sql.safe_query("SELECT MIN(Value), MAX(Value) FROM Meter WHERE (DeviceRowID=%" PRIu64 " AND Date>='%q')", idx, szDateEnd);
						if (!result.empty())
						{
							root["result"][ii]["d"] = szDateEnd;
							root["result"][ii]["u_min"] = atof(result[0][0].c_str()) / 10.0F;
							root["result"][ii]["u_max"] = atof(result[0][1].c_str()) / 10.0F;
							ii++;
						}
					}
					else if (!bIsManagedCounter)
					{
						/*if (sgroupby == "year") {

						} else*/
						{
							// get the first value
							result = m_sql.safe_query(
								//"SELECT MIN(Value), MAX(Value) FROM Meter WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q')",
								"SELECT Value FROM Meter WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q') ORDER BY Date ASC LIMIT 1", idx, szDateEnd);
							if (!result.empty())
							{
								std::vector<std::string> sd = result[0];
								int64_t total_min = std::stoll(sd[0]);
								int64_t total_max = total_min;
								int64_t total_real;

								// Get the last value
								result = m_sql.safe_query("SELECT Value FROM Meter WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q') ORDER BY Date DESC LIMIT 1", idx,
									szDateEnd);
								if (!result.empty())
								{
									std::vector<std::string> sd = result[0];
									total_max = std::stoull(sd[0]);
								}

								total_real = total_max - total_min;
								sprintf(szTmp, "%" PRId64, total_real);

								std::string szValue = szTmp;

								if (!sgroupby.empty())
								{
									double todayValue = double(total_real) / divider;
									std::string formatString;
									switch (metertype)
									{
									case MTYPE_ENERGY:
									case MTYPE_ENERGY_GENERATED:
										formatString = "%.3f";
										break;
									case MTYPE_GAS:
										formatString = "%.2f";
										break;
									case MTYPE_WATER:
										formatString = "%.3f";
										break;
									case MTYPE_COUNTER:
										formatString = "%.10g";
										break;
									}
									AddTodayValueToResult(root, sgroupby, std::string(szDateEnd), todayValue, formatString);
								}
								else
								{
									root["result"][ii]["d"] = szDateEnd;
									switch (metertype)
									{
									case MTYPE_ENERGY:
									case MTYPE_ENERGY_GENERATED: {
										sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
										root["result"][ii]["v"] = szTmp;

										std::vector<std::string> mresults;
										StringSplit(sValue, ";", mresults);
										if (mresults.size() == 2)
										{
											sValue = mresults[1];
										}
										if (dType == pTypeENERGY)
											sprintf(szTmp, "%.3f", meteroffset + (((atof(sValue.c_str()) * 100.0F) - atof(szValue.c_str())) / divider));
										else
											sprintf(szTmp, "%.3f", meteroffset + ((atof(sValue.c_str()) - atof(szValue.c_str())) / divider));
										root["result"][ii]["c"] = szTmp;
									}
															   break;
									case MTYPE_GAS:
										sprintf(szTmp, "%.2f", atof(szValue.c_str()) / divider);
										root["result"][ii]["v"] = szTmp;
										sprintf(szTmp, "%.2f", meteroffset + ((atof(sValue.c_str()) - atof(szValue.c_str())) / divider));
										root["result"][ii]["c"] = szTmp;
										break;
									case MTYPE_WATER:
										sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
										root["result"][ii]["v"] = szTmp;
										sprintf(szTmp, "%.3f", meteroffset + ((atof(sValue.c_str()) - atof(szValue.c_str())) / divider));
										root["result"][ii]["c"] = szTmp;
										break;
									case MTYPE_COUNTER:
										sprintf(szTmp, "%.10g", atof(szValue.c_str()) / divider);
										root["result"][ii]["v"] = szTmp;
										sprintf(szTmp, "%.10g", meteroffset + ((atof(sValue.c_str()) - atof(szValue.c_str())) / divider));
										root["result"][ii]["c"] = szTmp;
										break;
									}
									ii++;
								}
							}
						}
					}
				}
				else if (sensor == "wind")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					int ii = 0;

					result = m_sql.safe_query("SELECT Direction, Speed_Min, Speed_Max, Gust_Min,"
						" Gust_Max, Date "
						"FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q'"
						" AND Date<='%q') ORDER BY Date ASC",
						dbasetable.c_str(), idx, szDateStart, szDateEnd);
					if (!result.empty())
					{
						for (const auto& sd : result)
						{
							root["result"][ii]["d"] = sd[5].substr(0, 16);
							root["result"][ii]["di"] = sd[0];

							int intSpeed = atoi(sd[2].c_str());
							int intGust = atoi(sd[4].c_str());
							if (m_sql.m_windunit != WINDUNIT_Beaufort)
							{
								sprintf(szTmp, "%.1f", float(intSpeed) * m_sql.m_windscale);
								root["result"][ii]["sp"] = szTmp;
								sprintf(szTmp, "%.1f", float(intGust) * m_sql.m_windscale);
								root["result"][ii]["gu"] = szTmp;
							}
							else
							{
								float windspeedms = float(intSpeed) * 0.1F;
								float windgustms = float(intGust) * 0.1F;
								sprintf(szTmp, "%d", MStoBeaufort(windspeedms));
								root["result"][ii]["sp"] = szTmp;
								sprintf(szTmp, "%d", MStoBeaufort(windgustms));
								root["result"][ii]["gu"] = szTmp;
							}
							ii++;
						}
					}
					// add today (have to calculate it)
					result = m_sql.safe_query("SELECT AVG(Direction), MIN(Speed), MAX(Speed),"
						" MIN(Gust), MAX(Gust) "
						"FROM Wind WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q') ORDER BY Date ASC",
						idx, szDateEnd);
					if (!result.empty())
					{
						std::vector<std::string> sd = result[0];

						root["result"][ii]["d"] = szDateEnd;
						root["result"][ii]["di"] = sd[0];

						int intSpeed = atoi(sd[2].c_str());
						int intGust = atoi(sd[4].c_str());
						if (m_sql.m_windunit != WINDUNIT_Beaufort)
						{
							sprintf(szTmp, "%.1f", float(intSpeed) * m_sql.m_windscale);
							root["result"][ii]["sp"] = szTmp;
							sprintf(szTmp, "%.1f", float(intGust) * m_sql.m_windscale);
							root["result"][ii]["gu"] = szTmp;
						}
						else
						{
							float windspeedms = float(intSpeed) * 0.1F;
							float windgustms = float(intGust) * 0.1F;
							sprintf(szTmp, "%d", MStoBeaufort(windspeedms));
							root["result"][ii]["sp"] = szTmp;
							sprintf(szTmp, "%d", MStoBeaufort(windgustms));
							root["result"][ii]["gu"] = szTmp;
						}
						ii++;
					}
					// Previous Year
					result = m_sql.safe_query("SELECT Direction, Speed_Min, Speed_Max, Gust_Min,"
						" Gust_Max, Date "
						"FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q'"
						" AND Date<='%q') ORDER BY Date ASC",
						dbasetable.c_str(), idx, szDateStartPrev, szDateEndPrev);
					if (!result.empty())
					{
						iPrev = 0;
						for (const auto& sd : result)
						{
							root["resultprev"][iPrev]["d"] = sd[5].substr(0, 16);
							root["resultprev"][iPrev]["di"] = sd[0];

							int intSpeed = atoi(sd[2].c_str());
							int intGust = atoi(sd[4].c_str());
							if (m_sql.m_windunit != WINDUNIT_Beaufort)
							{
								sprintf(szTmp, "%.1f", float(intSpeed) * m_sql.m_windscale);
								root["resultprev"][iPrev]["sp"] = szTmp;
								sprintf(szTmp, "%.1f", float(intGust) * m_sql.m_windscale);
								root["resultprev"][iPrev]["gu"] = szTmp;
							}
							else
							{
								float windspeedms = float(intSpeed) * 0.1F;
								float windgustms = float(intGust) * 0.1F;
								sprintf(szTmp, "%d", MStoBeaufort(windspeedms));
								root["resultprev"][iPrev]["sp"] = szTmp;
								sprintf(szTmp, "%d", MStoBeaufort(windgustms));
								root["resultprev"][iPrev]["gu"] = szTmp;
							}
							iPrev++;
						}
					}
				}
			}													 // month or year
			else if ((srange.substr(0, 1) == "2") && (srange.substr(10, 1) == "T") && (srange.substr(11, 1) == "2")) // custom range 2013-01-01T2013-12-31
			{
				std::string szDateStart = srange.substr(0, 10);
				std::string szDateEnd = srange.substr(11, 10);
				std::string sgraphtype = request::findValue(&req, "graphtype");
				std::string sgraphTemp = request::findValue(&req, "graphTemp");
				std::string sgraphChill = request::findValue(&req, "graphChill");
				std::string sgraphHum = request::findValue(&req, "graphHum");
				std::string sgraphBaro = request::findValue(&req, "graphBaro");
				std::string sgraphDew = request::findValue(&req, "graphDew");
				std::string sgraphSet = request::findValue(&req, "graphSet");

				if (sensor == "temp")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					bool sendTemp = false;
					bool sendChill = false;
					bool sendHum = false;
					bool sendBaro = false;
					bool sendDew = false;
					bool sendSet = false;

					if ((sgraphTemp == "true") &&
						((dType == pTypeRego6XXTemp) || (dType == pTypeTEMP) || (dType == pTypeTEMP_HUM) || (dType == pTypeTEMP_HUM_BARO) || (dType == pTypeTEMP_BARO) ||
							(dType == pTypeWIND) || (dType == pTypeThermostat1) || (dType == pTypeRadiator1) || ((dType == pTypeUV) && (dSubType == sTypeUV3)) ||
							((dType == pTypeWIND) && (dSubType == sTypeWIND4)) || ((dType == pTypeRFXSensor) && (dSubType == sTypeRFXSensorTemp)) ||
							((dType == pTypeThermostat) && (dSubType == sTypeThermSetpoint)) || (dType == pTypeEvohomeZone) || (dType == pTypeEvohomeWater)))
					{
						sendTemp = true;
					}
					if ((sgraphSet == "true") && ((dType == pTypeEvohomeZone) || (dType == pTypeEvohomeWater))) // FIXME cheat for water setpoint is just on or off
					{
						sendSet = true;
					}
					if ((sgraphChill == "true") && (((dType == pTypeWIND) && (dSubType == sTypeWIND4)) || ((dType == pTypeWIND) && (dSubType == sTypeWINDNoTemp))))
					{
						sendChill = true;
					}
					if ((sgraphHum == "true") && ((dType == pTypeHUM) || (dType == pTypeTEMP_HUM) || (dType == pTypeTEMP_HUM_BARO)))
					{
						sendHum = true;
					}
					if ((sgraphBaro == "true") && ((dType == pTypeTEMP_HUM_BARO) || (dType == pTypeTEMP_BARO) || ((dType == pTypeGeneral) && (dSubType == sTypeBaro))))
					{
						sendBaro = true;
					}
					if ((sgraphDew == "true") && ((dType == pTypeTEMP_HUM) || (dType == pTypeTEMP_HUM_BARO)))
					{
						sendDew = true;
					}

					if (sgraphtype == "1")
					{
						// Need to get all values of the end date so 23:59:59 is appended to the date string
						result = m_sql.safe_query("SELECT Temperature, Chill, Humidity, Barometer,"
							" Date, DewPoint, SetPoint "
							"FROM Temperature WHERE (DeviceRowID==%" PRIu64 ""
							" AND Date>='%q' AND Date<='%q 23:59:59') ORDER BY Date ASC",
							idx, szDateStart.c_str(), szDateEnd.c_str());
						int ii = 0;
						if (!result.empty())
						{
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[4]; //.substr(0,16);
								if (sendTemp)
								{
									double te = ConvertTemperature(atof(sd[0].c_str()), tempsign);
									double tm = ConvertTemperature(atof(sd[0].c_str()), tempsign);
									root["result"][ii]["te"] = te;
									root["result"][ii]["tm"] = tm;
								}
								if (sendChill)
								{
									double ch = ConvertTemperature(atof(sd[1].c_str()), tempsign);
									double cm = ConvertTemperature(atof(sd[1].c_str()), tempsign);
									root["result"][ii]["ch"] = ch;
									root["result"][ii]["cm"] = cm;
								}
								if (sendHum)
								{
									root["result"][ii]["hu"] = sd[2];
								}
								if (sendBaro)
								{
									if (dType == pTypeTEMP_HUM_BARO)
									{
										if (dSubType == sTypeTHBFloat)
										{
											sprintf(szTmp, "%.1f", atof(sd[3].c_str()) / 10.0F);
											root["result"][ii]["ba"] = szTmp;
										}
										else
											root["result"][ii]["ba"] = sd[3];
									}
									else if (dType == pTypeTEMP_BARO)
									{
										sprintf(szTmp, "%.1f", atof(sd[3].c_str()) / 10.0F);
										root["result"][ii]["ba"] = szTmp;
									}
									else if ((dType == pTypeGeneral) && (dSubType == sTypeBaro))
									{
										sprintf(szTmp, "%.1f", atof(sd[3].c_str()) / 10.0F);
										root["result"][ii]["ba"] = szTmp;
									}
								}
								if (sendDew)
								{
									double dp = ConvertTemperature(atof(sd[5].c_str()), tempsign);
									root["result"][ii]["dp"] = dp;
								}
								if (sendSet)
								{
									double se = ConvertTemperature(atof(sd[6].c_str()), tempsign);
									root["result"][ii]["se"] = se;
								}
								ii++;
							}
						}
					}
					else
					{
						result = m_sql.safe_query("SELECT Temp_Min, Temp_Max, Chill_Min, Chill_Max,"
							" Humidity, Barometer, Date, DewPoint, Temp_Avg,"
							" SetPoint_Min, SetPoint_Max, SetPoint_Avg "
							"FROM Temperature_Calendar "
							"WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q'"
							" AND Date<='%q') ORDER BY Date ASC",
							idx, szDateStart.c_str(), szDateEnd.c_str());
						int ii = 0;
						if (!result.empty())
						{
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[6].substr(0, 16);
								if (sendTemp)
								{
									double te = ConvertTemperature(atof(sd[1].c_str()), tempsign);
									double tm = ConvertTemperature(atof(sd[0].c_str()), tempsign);
									double ta = ConvertTemperature(atof(sd[8].c_str()), tempsign);

									root["result"][ii]["te"] = te;
									root["result"][ii]["tm"] = tm;
									root["result"][ii]["ta"] = ta;
								}
								if (sendChill)
								{
									double ch = ConvertTemperature(atof(sd[3].c_str()), tempsign);
									double cm = ConvertTemperature(atof(sd[2].c_str()), tempsign);

									root["result"][ii]["ch"] = ch;
									root["result"][ii]["cm"] = cm;
								}
								if (sendHum)
								{
									root["result"][ii]["hu"] = sd[4];
								}
								if (sendBaro)
								{
									if (dType == pTypeTEMP_HUM_BARO)
									{
										if (dSubType == sTypeTHBFloat)
										{
											sprintf(szTmp, "%.1f", atof(sd[5].c_str()) / 10.0F);
											root["result"][ii]["ba"] = szTmp;
										}
										else
											root["result"][ii]["ba"] = sd[5];
									}
									else if (dType == pTypeTEMP_BARO)
									{
										sprintf(szTmp, "%.1f", atof(sd[5].c_str()) / 10.0F);
										root["result"][ii]["ba"] = szTmp;
									}
									else if ((dType == pTypeGeneral) && (dSubType == sTypeBaro))
									{
										sprintf(szTmp, "%.1f", atof(sd[5].c_str()) / 10.0F);
										root["result"][ii]["ba"] = szTmp;
									}
								}
								if (sendDew)
								{
									double dp = ConvertTemperature(atof(sd[7].c_str()), tempsign);
									root["result"][ii]["dp"] = dp;
								}
								if (sendSet)
								{
									double sm = ConvertTemperature(atof(sd[9].c_str()), tempsign);
									double sx = ConvertTemperature(atof(sd[10].c_str()), tempsign);
									double se = ConvertTemperature(atof(sd[11].c_str()), tempsign);
									root["result"][ii]["sm"] = sm;
									root["result"][ii]["se"] = se;
									root["result"][ii]["sx"] = sx;
									char szTmp[1024];
									sprintf(szTmp, "%.1f %.1f %.1f", sm, se, sx);
									_log.Log(LOG_STATUS, "%s", szTmp);
								}
								ii++;
							}
						}

						// add today (have to calculate it)
						result = m_sql.safe_query("SELECT MIN(Temperature), MAX(Temperature),"
							" MIN(Chill), MAX(Chill), AVG(Humidity),"
							" AVG(Barometer), MIN(DewPoint), AVG(Temperature),"
							" MIN(SetPoint), MAX(SetPoint), AVG(SetPoint) "
							"FROM Temperature WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q')",
							idx, szDateEnd.c_str());
						if (!result.empty())
						{
							std::vector<std::string> sd = result[0];

							root["result"][ii]["d"] = szDateEnd;
							if (sendTemp)
							{
								double te = ConvertTemperature(atof(sd[1].c_str()), tempsign);
								double tm = ConvertTemperature(atof(sd[0].c_str()), tempsign);
								double ta = ConvertTemperature(atof(sd[7].c_str()), tempsign);

								root["result"][ii]["te"] = te;
								root["result"][ii]["tm"] = tm;
								root["result"][ii]["ta"] = ta;
							}
							if (sendChill)
							{
								double ch = ConvertTemperature(atof(sd[3].c_str()), tempsign);
								double cm = ConvertTemperature(atof(sd[2].c_str()), tempsign);
								root["result"][ii]["ch"] = ch;
								root["result"][ii]["cm"] = cm;
							}
							if (sendHum)
							{
								root["result"][ii]["hu"] = sd[4];
							}
							if (sendBaro)
							{
								if (dType == pTypeTEMP_HUM_BARO)
								{
									if (dSubType == sTypeTHBFloat)
									{
										sprintf(szTmp, "%.1f", atof(sd[5].c_str()) / 10.0F);
										root["result"][ii]["ba"] = szTmp;
									}
									else
										root["result"][ii]["ba"] = sd[5];
								}
								else if (dType == pTypeTEMP_BARO)
								{
									sprintf(szTmp, "%.1f", atof(sd[5].c_str()) / 10.0F);
									root["result"][ii]["ba"] = szTmp;
								}
								else if ((dType == pTypeGeneral) && (dSubType == sTypeBaro))
								{
									sprintf(szTmp, "%.1f", atof(sd[5].c_str()) / 10.0F);
									root["result"][ii]["ba"] = szTmp;
								}
							}
							if (sendDew)
							{
								double dp = ConvertTemperature(atof(sd[6].c_str()), tempsign);
								root["result"][ii]["dp"] = dp;
							}
							if (sendSet)
							{
								double sm = ConvertTemperature(atof(sd[8].c_str()), tempsign);
								double sx = ConvertTemperature(atof(sd[9].c_str()), tempsign);
								double se = ConvertTemperature(atof(sd[10].c_str()), tempsign);

								root["result"][ii]["sm"] = sm;
								root["result"][ii]["se"] = se;
								root["result"][ii]["sx"] = sx;
							}
							ii++;
						}
					}
				}
				else if (sensor == "uv")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					result = m_sql.safe_query("SELECT Level, Date FROM %s WHERE (DeviceRowID==%" PRIu64 ""
						" AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
						dbasetable.c_str(), idx, szDateStart.c_str(), szDateEnd.c_str());
					int ii = 0;
					if (!result.empty())
					{
						for (const auto& sd : result)
						{
							root["result"][ii]["d"] = sd[1].substr(0, 16);
							root["result"][ii]["uvi"] = sd[0];
							ii++;
						}
					}
					// add today (have to calculate it)
					result = m_sql.safe_query("SELECT MAX(Level) FROM UV WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q')", idx, szDateEnd.c_str());
					if (!result.empty())
					{
						std::vector<std::string> sd = result[0];

						root["result"][ii]["d"] = szDateEnd;
						root["result"][ii]["uvi"] = sd[0];
						ii++;
					}
				}
				else if (sensor == "rain")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					result = m_sql.safe_query("SELECT Total, Rate, Date FROM %s "
						"WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
						dbasetable.c_str(), idx, szDateStart.c_str(), szDateEnd.c_str());
					int ii = 0;
					if (!result.empty())
					{
						for (const auto& sd : result)
						{
							root["result"][ii]["d"] = sd[2].substr(0, 16);
							root["result"][ii]["mm"] = sd[0];
							ii++;
						}
					}
					// add today (have to calculate it)
					if (dSubType == sTypeRAINWU || dSubType == sTypeRAINByRate)
					{
						result = m_sql.safe_query("SELECT Total, Total, Rate FROM Rain WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q') ORDER BY ROWID DESC LIMIT 1", idx,
							szDateEnd.c_str());
					}
					else
					{
						result = m_sql.safe_query("SELECT MIN(Total), MAX(Total), MAX(Rate) FROM Rain WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q')", idx, szDateEnd.c_str());
					}
					if (!result.empty())
					{
						std::vector<std::string> sd = result[0];

						float total_min = static_cast<float>(atof(sd[0].c_str()));
						float total_max = static_cast<float>(atof(sd[1].c_str()));
						// int rate = atoi(sd[2].c_str());

						float total_real = 0;
						if (dSubType == sTypeRAINWU || dSubType == sTypeRAINByRate)
						{
							total_real = total_max;
						}
						else
						{
							total_real = total_max - total_min;
						}
						sprintf(szTmp, "%.1f", total_real);
						root["result"][ii]["d"] = szDateEnd;
						root["result"][ii]["mm"] = szTmp;
						ii++;
					}
				}
				else if (sensor == "counter")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;
					root["ValueQuantity"] = options["ValueQuantity"];
					root["ValueUnits"] = options["ValueUnits"];
					root["Divider"] = divider;

					int ii = 0;
					if (dType == pTypeP1Power)
					{
						result = m_sql.safe_query("SELECT Value1,Value2,Value5,Value6, Date "
							"FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q'"
							" AND Date<='%q') ORDER BY Date ASC",
							dbasetable.c_str(), idx, szDateStart.c_str(), szDateEnd.c_str());
						if (!result.empty())
						{
							bool bHaveDeliverd = false;
							for (const auto& sd : result)
							{
								root["result"][ii]["d"] = sd[4].substr(0, 16);

								std::string szUsage1 = sd[0];
								std::string szDeliv1 = sd[1];
								std::string szUsage2 = sd[2];
								std::string szDeliv2 = sd[3];

								float fUsage = (float)(atof(szUsage1.c_str()) + atof(szUsage2.c_str()));
								float fDeliv = (float)(atof(szDeliv1.c_str()) + atof(szDeliv2.c_str()));

								if (fDeliv != 0)
									bHaveDeliverd = true;
								sprintf(szTmp, "%.3f", fUsage / divider);
								root["result"][ii]["v"] = szTmp;
								sprintf(szTmp, "%.3f", fDeliv / divider);
								root["result"][ii]["v2"] = szTmp;
								ii++;
							}
							if (bHaveDeliverd)
							{
								root["delivered"] = true;
							}
						}
					}
					else
					{
						result = m_sql.safe_query("SELECT Value, Date FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q' AND Date<='%q') ORDER BY Date ASC",
							dbasetable.c_str(), idx, szDateStart.c_str(), szDateEnd.c_str());
						if (!result.empty())
						{
							for (const auto& sd : result)
							{
								std::string szValue = sd[0];
								switch (metertype)
								{
								case MTYPE_ENERGY:
								case MTYPE_ENERGY_GENERATED:
									sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
									szValue = szTmp;
									break;
								case MTYPE_GAS:
									sprintf(szTmp, "%.2f", atof(szValue.c_str()) / divider);
									szValue = szTmp;
									break;
								case MTYPE_WATER:
									sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
									szValue = szTmp;
									break;
								case MTYPE_COUNTER:
									sprintf(szTmp, "%.10g", atof(szValue.c_str()) / divider);
									szValue = szTmp;
									break;

								}
								root["result"][ii]["d"] = sd[1].substr(0, 16);
								root["result"][ii]["v"] = szValue;
								ii++;
							}
						}
					}
					// add today (have to calculate it)
					if (dType == pTypeP1Power)
					{
						result = m_sql.safe_query("SELECT MIN(Value1), MAX(Value1), MIN(Value2),"
							" MAX(Value2),MIN(Value5), MAX(Value5),"
							" MIN(Value6), MAX(Value6) "
							"FROM MultiMeter WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q')",
							idx, szDateEnd.c_str());
						bool bHaveDeliverd = false;
						if (!result.empty())
						{
							std::vector<std::string> sd = result[0];

							uint64_t total_min_usage_1 = std::stoull(sd[0]);
							uint64_t total_max_usage_1 = std::stoull(sd[1]);
							uint64_t total_min_usage_2 = std::stoull(sd[4]);
							uint64_t total_max_usage_2 = std::stoull(sd[5]);
							uint64_t total_real_usage;

							uint64_t total_min_deliv_1 = std::stoull(sd[2]);
							uint64_t total_max_deliv_1 = std::stoull(sd[3]);
							uint64_t total_min_deliv_2 = std::stoull(sd[6]);
							uint64_t total_max_deliv_2 = std::stoull(sd[7]);
							uint64_t total_real_deliv;

							total_real_usage = (total_max_usage_1 + total_max_usage_2) - (total_min_usage_1 + total_min_usage_2);
							total_real_deliv = (total_max_deliv_1 + total_max_deliv_2) - (total_min_deliv_1 + total_min_deliv_2);

							if (total_real_deliv != 0)
								bHaveDeliverd = true;

							root["result"][ii]["d"] = szDateEnd;

							sprintf(szTmp, "%" PRIu64, total_real_usage);
							std::string szValue = szTmp;
							sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
							root["result"][ii]["v"] = szTmp;

							sprintf(szTmp, "%" PRIu64, total_real_deliv);
							szValue = szTmp;
							sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
							root["result"][ii]["v2"] = szTmp;
							
							ii++;
							if (bHaveDeliverd)
							{
								root["delivered"] = true;
							}
						}
					}
					else if (!bIsManagedCounter)
					{ // get the first value of the day
						result = m_sql.safe_query(
							//"SELECT MIN(Value), MAX(Value) FROM Meter WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q')",
							"SELECT Value FROM Meter WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q') ORDER BY Date ASC LIMIT 1", idx, szDateEnd.c_str());
						if (!result.empty())
						{
							std::vector<std::string> sd = result[0];
							int64_t total_min = std::stoll(sd[0]);
							int64_t total_max = total_min;
							int64_t total_real;

							// get the last value of the day
							result = m_sql.safe_query("SELECT Value FROM Meter WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q') ORDER BY Date DESC LIMIT 1", idx,
								szDateEnd.c_str());
							if (!result.empty())
							{
								std::vector<std::string> sd = result[0];
								total_max = std::stoull(sd[0]);
							}

							total_real = total_max - total_min;
							sprintf(szTmp, "%" PRId64, total_real);							std::string szValue = szTmp;

							switch (metertype)
							{
							case MTYPE_ENERGY:
							case MTYPE_ENERGY_GENERATED:
								sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
								szValue = szTmp;
								break;
							case MTYPE_GAS:
								sprintf(szTmp, "%.2f", atof(szValue.c_str()) / divider);
								szValue = szTmp;
								break;
							case MTYPE_WATER:
								sprintf(szTmp, "%.3f", atof(szValue.c_str()) / divider);
								szValue = szTmp;
								break;
							case MTYPE_COUNTER:
								sprintf(szTmp, "%.10g", atof(szValue.c_str()) / divider);
								szValue = szTmp;
								break;
							}

							root["result"][ii]["d"] = szDateEnd;
							root["result"][ii]["v"] = szValue;
							ii++;
						}
					}
				}
				else if (sensor == "wind")
				{
					root["status"] = "OK";
					root["title"] = "Graph " + sensor + " " + srange;

					int ii = 0;

					result = m_sql.safe_query("SELECT Direction, Speed_Min, Speed_Max, Gust_Min,"
						" Gust_Max, Date "
						"FROM %s WHERE (DeviceRowID==%" PRIu64 " AND Date>='%q'"
						" AND Date<='%q') ORDER BY Date ASC",
						dbasetable.c_str(), idx, szDateStart.c_str(), szDateEnd.c_str());
					if (!result.empty())
					{
						for (const auto& sd : result)
						{
							root["result"][ii]["d"] = sd[5].substr(0, 16);
							root["result"][ii]["di"] = sd[0];

							int intSpeed = atoi(sd[2].c_str());
							int intGust = atoi(sd[4].c_str());
							if (m_sql.m_windunit != WINDUNIT_Beaufort)
							{
								sprintf(szTmp, "%.1f", float(intSpeed) * m_sql.m_windscale);
								root["result"][ii]["sp"] = szTmp;
								sprintf(szTmp, "%.1f", float(intGust) * m_sql.m_windscale);
								root["result"][ii]["gu"] = szTmp;
							}
							else
							{
								float windspeedms = float(intSpeed) * 0.1F;
								float windgustms = float(intGust) * 0.1F;
								sprintf(szTmp, "%d", MStoBeaufort(windspeedms));
								root["result"][ii]["sp"] = szTmp;
								sprintf(szTmp, "%d", MStoBeaufort(windgustms));
								root["result"][ii]["gu"] = szTmp;
							}
							ii++;
						}
					}
					// add today (have to calculate it)
					result = m_sql.safe_query("SELECT AVG(Direction), MIN(Speed), MAX(Speed), MIN(Gust), MAX(Gust) FROM Wind WHERE (DeviceRowID==%" PRIu64
						" AND Date>='%q') ORDER BY Date ASC",
						idx, szDateEnd.c_str());
					if (!result.empty())
					{
						std::vector<std::string> sd = result[0];

						root["result"][ii]["d"] = szDateEnd;
						root["result"][ii]["di"] = sd[0];

						int intSpeed = atoi(sd[2].c_str());
						int intGust = atoi(sd[4].c_str());
						if (m_sql.m_windunit != WINDUNIT_Beaufort)
						{
							sprintf(szTmp, "%.1f", float(intSpeed) * m_sql.m_windscale);
							root["result"][ii]["sp"] = szTmp;
							sprintf(szTmp, "%.1f", float(intGust) * m_sql.m_windscale);
							root["result"][ii]["gu"] = szTmp;
						}
						else
						{
							float windspeedms = float(intSpeed) * 0.1F;
							float windgustms = float(intGust) * 0.1F;
							sprintf(szTmp, "%d", MStoBeaufort(windspeedms));
							root["result"][ii]["sp"] = szTmp;
							sprintf(szTmp, "%d", MStoBeaufort(windgustms));
							root["result"][ii]["gu"] = szTmp;
						}
						ii++;
					}
				}
			} // custom range
		}

		/*
		 * Takes root["result"] and groups all items according to sgroupby, summing all values for each category, then creating new items in root["result"]
		 * for each combination year/category.
		 */
		void CWebServer::GroupBy(Json::Value& root, std::string dbasetable, uint64_t idx, std::string sgroupby, std::function<std::string(std::string)> counter,
			std::function<std::string(std::string)> value, std::function<std::string(double)> sumToResult)
		{
			/*
			 * This query selects all records (in mc0) that belong to DeviceRowID, each with the record before it (in mc1), and calculates for each record
			 * the "usage" by subtracting the previous counter from its counter.
			 * - It does not take into account records that have a 0-valued counter, to prevent one falling between two categories, which would cause the
			 *   value for one category to be extremely low and the value for the other extremely high.
			 * - When the previous counter is greater than its counter, assumed is that a meter change has taken place; the previous counter is ignored
			 *   and the value of the record is taken as the "usage" (hoping for the best as the value is not always reliable.)
			 * - The reason why not simply the record values are summed, but instead the differences between all the individual counters are summed, is that
			 *   records for some days are not recorded or sometimes disappear, hence values would be missing and that would result in an incomplete total.
			 *   Plus it seems that the value is not always the same as the difference between the counters. Counters are more often reliable.
			 */
			std::string queryString;
			queryString.append(" select");
			queryString.append("  strftime('%%Y',Date) as Year,");
			queryString.append("  sum(Difference) as Sum");
			if (sgroupby == "quarter")
			{
				queryString.append(",case");
				queryString.append("   when cast(strftime('%%m',Date) as integer) between 1 and 3 then 'Q1'");
				queryString.append("   when cast(strftime('%%m',Date) as integer) between 4 and 6 then 'Q2'");
				queryString.append("   when cast(strftime('%%m',Date) as integer) between 7 and 9 then 'Q3'");
				queryString.append("                                                              else 'Q4'");
				queryString.append("   end as Quarter");
			}
			else if (sgroupby == "month")
			{
				queryString.append(",strftime('%%m',Date) as Month");
			}
			queryString.append(" from (");
			queryString.append(" 	select");
			queryString.append("         mc0.DeviceRowID,");
			queryString.append("         date(mc0.Date) as Date,");
			queryString.append("         case");
			queryString.append("            when (" + counter("mc1") + ") <= (" + counter("mc0") + ")");
			queryString.append("            then (" + counter("mc0") + ") - (" + counter("mc1") + ")");
			queryString.append("            else (" + value("mc0") + ")");
			queryString.append("         end as Difference");
			queryString.append(" 	from " + dbasetable + " mc0");
			queryString.append(" 	inner join " + dbasetable + " mc1 on mc1.DeviceRowID = mc0.DeviceRowID");
			queryString.append("         and mc1.Date = (");
			queryString.append("             select max(mcm.Date)");
			queryString.append("             from " + dbasetable + " mcm");
			queryString.append("             where mcm.DeviceRowID = mc0.DeviceRowID and mcm.Date < mc0.Date and (" + counter("mcm") + ") > 0");
			queryString.append("         )");
			queryString.append(" 	where");
			queryString.append("         mc0.DeviceRowID = %" PRIu64 "");
			queryString.append("         and (" + counter("mc0") + ") > 0");
			queryString.append("         and (select min(Date) from " + dbasetable + " where DeviceRowID = %" PRIu64 " and (" + counter("") + ") > 0) <= mc1.Date");
			queryString.append("         and mc0.Date <= (select max(Date) from " + dbasetable + " where DeviceRowID = %" PRIu64 " and (" + counter("") + ") > 0)");
			queryString.append("    union all");
			queryString.append("    select");
			queryString.append("         DeviceRowID,");
			queryString.append("         date(Date) as Date,");
			queryString.append("         " + value(""));
			queryString.append(" 	from " + dbasetable);
			queryString.append(" 	where");
			queryString.append("         DeviceRowID = %" PRIu64 "");
			queryString.append("         and (select min(Date) from " + dbasetable + " where DeviceRowID = %" PRIu64 " and (" + counter("") + ") > 0) = Date");
			queryString.append(" )");
			queryString.append(" group by strftime('%%Y',Date)");
			if (sgroupby == "quarter")
			{
				queryString.append(",case");
				queryString.append("   when cast(strftime('%%m',Date) as integer) between 1 and 3 then 'Q1'");
				queryString.append("   when cast(strftime('%%m',Date) as integer) between 4 and 6 then 'Q2'");
				queryString.append("   when cast(strftime('%%m',Date) as integer) between 7 and 9 then 'Q3'");
				queryString.append("                                                              else 'Q4'");
				queryString.append("   end");
			}
			else if (sgroupby == "month")
			{
				queryString.append(",strftime('%%m',Date)");
			}
			std::vector<std::vector<std::string>> result = m_sql.safe_query(queryString.c_str(), idx, idx, idx, idx, idx);
			if (!result.empty())
			{
				int firstYearCounting = 0;
				double yearSumPrevious[12];
				int yearPrevious[12];
				for (const auto& sd : result)
				{
					const int year = atoi(sd[0].c_str());
					const double fsum = atof(sd[1].c_str());
					const int previousIndex = sgroupby == "year" ? 0 : sgroupby == "quarter" ? sd[2][1] - '0' - 1 : atoi(sd[2].c_str()) - 1;
					const double* sumPrevious = year - 1 != yearPrevious[previousIndex] ? NULL : &yearSumPrevious[previousIndex];
					const char* trend = !sumPrevious ? "" : *sumPrevious < fsum ? "up" : *sumPrevious > fsum ? "down" : "equal";
					const int ii = root["result"].size();
					if (firstYearCounting == 0 || year < firstYearCounting)
					{
						firstYearCounting = year;
					}
					root["result"][ii]["y"] = sd[0];
					root["result"][ii]["c"] = sgroupby == "year" ? sd[0] : sd[2];
					root["result"][ii]["s"] = sumToResult(fsum);
					root["result"][ii]["t"] = trend;
					yearSumPrevious[previousIndex] = fsum;
					yearPrevious[previousIndex] = year;
				}
				root["firstYear"] = firstYearCounting;
			}
		}

		/*
		 * Adds todayValue to root["result"], either by adding it to the value of the item with the corresponding category or by adding a new item with the
		 * respective category with todayValue. If root["firstYear"] is missing, the today's year is set in it's place.
		 */
		void CWebServer::AddTodayValueToResult(Json::Value& root, const std::string& sgroupby, const std::string& today, const double todayValue, const std::string& formatString)
		{
			std::string todayYear = today.substr(0, 4);
			std::string todayCategory;
			if (sgroupby == "quarter")
			{
				int todayMonth = atoi(today.substr(5, 2).c_str());
				if (todayMonth < 4)
					todayCategory = "Q1";
				else if (todayMonth < 7)
					todayCategory = "Q2";
				else if (todayMonth < 10)
					todayCategory = "Q3";
				else
					todayCategory = "Q4";
			}
			else if (sgroupby == "month")
			{
				todayCategory = today.substr(5, 2);
			}
			else
			{
				todayCategory = todayYear;
			}
			int todayResultIndex = -1;
			for (int resultIndex = 0; resultIndex < static_cast<int>(root["result"].size()) && todayResultIndex == -1; resultIndex++)
			{
				std::string resultYear = root["result"][resultIndex]["y"].asString();
				std::string resultCategory = root["result"][resultIndex]["c"].asString();
				if (resultYear == todayYear && todayCategory == resultCategory)
				{
					todayResultIndex = resultIndex;
				}
			}
			double resultPlusTodayValue = 0;
			if (todayResultIndex == -1)
			{
				todayResultIndex = root["result"].size();
				resultPlusTodayValue = todayValue;
				root["result"][todayResultIndex]["y"] = todayYear.c_str();
				root["result"][todayResultIndex]["c"] = todayCategory.c_str();
			}
			else
			{
				resultPlusTodayValue = atof(root["result"][todayResultIndex]["s"].asString().c_str()) + todayValue;
			}
			char szTmp[30];
			sprintf(szTmp, formatString.c_str(), resultPlusTodayValue);
			root["result"][todayResultIndex]["s"] = szTmp;

			if (!root.isMember("firstYear")) {
				root["firstYear"] = todayYear.c_str();
			}
		}

		/**
		 * Retrieve user session from store, without remote host.
		 */
		WebEmStoredSession CWebServer::GetSession(const std::string& sessionId)
		{
			_log.Debug(DEBUG_AUTH, "SessionStore : get...(%s)", sessionId.c_str());
			WebEmStoredSession session;

			if (sessionId.empty())
			{
				_log.Log(LOG_ERROR, "SessionStore : cannot get session without id.");
			}
			else
			{
				std::vector<std::vector<std::string>> result;
				result = m_sql.safe_query("SELECT SessionID, Username, AuthToken, ExpirationDate FROM UserSessions WHERE SessionID = '%q'", sessionId.c_str());
				if (!result.empty())
				{
					session.id = result[0][0];
					session.username = base64_decode(result[0][1]);
					session.auth_token = result[0][2];

					std::string sExpirationDate = result[0][3];
					// time_t now = mytime(NULL);
					struct tm tExpirationDate;
					ParseSQLdatetime(session.expires, tExpirationDate, sExpirationDate);
					// RemoteHost is not used to restore the session
					// LastUpdate is not used to restore the session
				}
				else
				{
					_log.Debug(DEBUG_AUTH, "SessionStore : session not Found! (%s)", sessionId.c_str());
				}
			}

			return session;
		}

		/**
		 * Save user session.
		 */
		void CWebServer::StoreSession(const WebEmStoredSession& session)
		{
			_log.Debug(DEBUG_AUTH, "SessionStore : store...(%s)", session.id.c_str());
			if (session.id.empty())
			{
				_log.Log(LOG_ERROR, "SessionStore : cannot store session without id.");
				return;
			}

			char szExpires[30];
			struct tm ltime;
			localtime_r(&session.expires, &ltime);
			strftime(szExpires, sizeof(szExpires), "%Y-%m-%d %H:%M:%S", &ltime);

			std::string remote_host = (session.remote_host.size() <= 50) ? // IPv4 : 15, IPv6 : (39|45)
				session.remote_host
				: session.remote_host.substr(0, 50);

			WebEmStoredSession storedSession = GetSession(session.id);
			if (storedSession.id.empty())
			{
				m_sql.safe_query("INSERT INTO UserSessions (SessionID, Username, AuthToken, ExpirationDate, RemoteHost) VALUES ('%q', '%q', '%q', '%q', '%q')", session.id.c_str(),
					base64_encode(session.username).c_str(), session.auth_token.c_str(), szExpires, remote_host.c_str());
			}
			else
			{
				m_sql.safe_query("UPDATE UserSessions set AuthToken = '%q', ExpirationDate = '%q', RemoteHost = '%q', LastUpdate = datetime('now', 'localtime') WHERE SessionID = '%q'",
					session.auth_token.c_str(), szExpires, remote_host.c_str(), session.id.c_str());
			}
		}

		/**
		 * Remove user session and expired sessions.
		 */
		void CWebServer::RemoveSession(const std::string& sessionId)
		{
			_log.Debug(DEBUG_AUTH, "SessionStore : remove... (%s)", sessionId.c_str());
			if (sessionId.empty())
			{
				return;
			}
			m_sql.safe_query("DELETE FROM UserSessions WHERE SessionID = '%q'", sessionId.c_str());
		}

		/**
		 * Remove all expired user sessions.
		 */
		void CWebServer::CleanSessions()
		{
			_log.Debug(DEBUG_AUTH, "SessionStore : clean...");
			m_sql.safe_query("DELETE FROM UserSessions WHERE ExpirationDate < datetime('now', 'localtime')");
		}

		/**
		 * Delete all user's session, except the session used to modify the username or password.
		 * username must have been hashed
		 */
		void CWebServer::RemoveUsersSessions(const std::string& username, const WebEmSession& exceptSession)
		{
			_log.Debug(DEBUG_AUTH, "SessionStore : remove all sessions for User... (%s)", exceptSession.id.c_str());
			m_sql.safe_query("DELETE FROM UserSessions WHERE (Username=='%q') and (SessionID!='%q')", username.c_str(), exceptSession.id.c_str());
		}

	} // namespace server
} // namespace http
