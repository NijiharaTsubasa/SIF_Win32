﻿/* 
   Copyright 2013 KLab Inc.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
#include "CKLBNetAPI.h"
#include "CKLBLuaEnv.h"
#include "CKLBUtility.h"
#include "CKLBJsonItem.h"
#include "CPFInterface.h"
#include "CKLBNetAPIKeyChain.h"
#include "SIF_Win32.h"
#include <time.h>
#include <ctype.h>

;

static int fail_times = 0;

enum {
	// Command Values
	NETAPI_STARTUP,				// start new account
	NETAPI_LOGIN,				// login to account
	NETAPI_LOGOUT,				// possibility unused
	NETAPI_SEND,				// send JSON packet
	NETAPI_CANCEL,				// selected session cancel
	NETAPI_CANCEL_ALL,			// abort all connection
	NETAPI_WATCH_MAINTENANCE,	// check for maintenance?
	NETAPI_DEBUG_HDR,			// unknwon
	NETAPI_GEN_CMDNUMID,		// create commandNum string in data. possibility unused
};

static IFactory::DEFCMD cmd[] = {
	{"NETAPI_STARTUP",					NETAPI_STARTUP					},
	{"NETAPI_LOGIN",					NETAPI_LOGIN					},
	{"NETAPI_LOGOUT",					NETAPI_LOGOUT					},
	{"NETAPI_SEND",						NETAPI_SEND						},
    {"NETAPI_CANCEL",					NETAPI_CANCEL					},
	{"NETAPI_CANCEL_ALL",				NETAPI_CANCEL_ALL				},
	{"NETAPI_WATCH_MAINTENANCE",		NETAPI_WATCH_MAINTENANCE		},
	{"NETAPI_DEBUG_HDR",				NETAPI_DEBUG_HDR				},
	{"NETAPI_GEN_CMDNUMID",				NETAPI_GEN_CMDNUMID				},

	//
	// Callback constants
	//
	{"NETAPIMSG_CONNECTION_CANCELED",	NETAPIMSG_CONNECTION_CANCELED	},
	{"NETAPIMSG_CONNECTION_FAILED",		NETAPIMSG_CONNECTION_FAILED		},
	{"NETAPIMSG_INVITE_FAILED",			NETAPIMSG_INVITE_FAILED			},
	{"NETAPIMSG_STARTUP_FAILED",		NETAPIMSG_STARTUP_FAILED		},
	{"NETAPIMSG_SERVER_TIMEOUT",		NETAPIMSG_SERVER_TIMEOUT		},
	{"NETAPIMSG_REQUEST_FAILED",		NETAPIMSG_REQUEST_FAILED		},
	{"NETAPIMSG_LOGIN_FAILED",			NETAPIMSG_LOGIN_FAILED			},
	{"NETAPIMSG_SERVER_ERROR",			NETAPIMSG_SERVER_ERROR			},
	{"NETAPIMSG_UNKNOWN",				NETAPIMSG_UNKNOWN				},
	{"NETAPIMSG_LOGIN_SUCCESS",			NETAPIMSG_LOGIN_SUCCESS			},
	{"NETAPIMSG_REQUEST_SUCCESS",		NETAPIMSG_REQUEST_SUCCESS		},
	{"NETAPIMSG_STARTUP_SUCCESS",		NETAPIMSG_STARTUP_SUCCESS		},
	{"NETAPIMSG_INVITE_SUCCESS",		NETAPIMSG_INVITE_SUCCESS		},
	{0, 0}
};

static CKLBTaskFactory<CKLBNetAPI> factory("HTTP_API", CLS_KLBNETAPI, cmd);

enum {
	ARG_CALLBACK	= 5,
	ARG_REQUIRE     = ARG_CALLBACK,
};

int map_netapi_success(int request_type)
{
	switch(request_type)
	{
		case NETAPI_STARTUP:
			return NETAPIMSG_STARTUP_SUCCESS;
		case NETAPI_LOGIN:
			return NETAPIMSG_LOGIN_SUCCESS;
		case NETAPI_SEND:
			return NETAPIMSG_REQUEST_SUCCESS;
		default:
			return NETAPIMSG_UNKNOWN;
	}
}

int map_netapi_fail(int request_type)
{
	switch(request_type)
	{
		case NETAPI_STARTUP:
			return NETAPIMSG_STARTUP_FAILED;
		case NETAPI_LOGIN:
			return NETAPIMSG_LOGIN_FAILED;
		case NETAPI_SEND:
			return NETAPIMSG_REQUEST_FAILED;
		default:
			return NETAPIMSG_SERVER_ERROR;
	}
}

char* create_authorize_string(const char* consumerKey, int nonce, const char* token = NULL)
{
	char* out = KLBNEWA(char, 256);

	if(token)
		sprintf(out, "consumerKey=%s&timeStamp=%d&version=1.1&token=%s&nonce=%d", consumerKey, int(time(NULL)), token, nonce);
	else
		sprintf(out, "consumerKey=%s&timeStamp=%d&version=1.1&nonce=%d", consumerKey, int(time(NULL)), nonce);

	return out;
}

CKLBNetAPI::CKLBNetAPI()
: CKLBLuaTask           ()
, m_http				(NULL)
, m_timeout				(30000)
, m_timestart			(0)
, m_canceled			(false)
, m_pRoot				(NULL)
, m_callback			(NULL)
, m_http_header_array	(NULL)
, m_http_header_length	(0)
, m_request_type		(-1)
, m_nonce				(1)
, m_netapi_phase		(0)
, m_downloading			(false)
{
	// Create the header array
}

CKLBNetAPI::~CKLBNetAPI() 
{
	// Done in Die()
}

u32 
CKLBNetAPI::getClassID() 
{
	return CLS_KLBNETAPI;
}

void CKLBNetAPI::startUp(int phase, int status_code)
{
	CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();

	// Startup phase
	switch(phase)
	{
		case 0:
		{
			// Do additional request. login/startUp
			char request_data[256];
			const char* form[2];
			char* authorize = create_authorize_string(kc.getConsumerKey(), m_nonce, kc.setToken(m_pRoot->child()->child()->getString()));
	
			// Create request_data
			sprintf(request_data, "request_data={\"login_key\":\"%s\",\"login_passwd\":\"%s\"}", kc.getLoginKey(), kc.getLoginPw());
			form[0] = request_data;
			form[1] = NULL;

			// Reuse m_http
			NetworkManager::releaseConnection(m_http);
			m_http = NetworkManager::createConnection();
			m_http->reuse();
			m_http->setForm(form);
			set_header(m_http, authorize);

			// Send
			char URL[MAX_PATH];

			sprintf(URL, "%s/login/startUp", kc.getURL());
			m_http->httpPOST(URL, false);

			// Set values
			m_netapi_phase++;
			m_timestart = 0;			// Reset

			KLBDELETEA(authorize);
			return;
		}
		case 1:
		{
			// Do additional request. login/startWithoutInvite
			char request_data[256];
			const char* form[2];
			char* authorize = create_authorize_string(kc.getConsumerKey(), m_nonce, kc.getToken());
	
			// Create request_data
			sprintf(request_data, "request_data={\"login_key\":\"%s\",\"login_passwd\":\"%s\"}", kc.getLoginKey(), kc.getLoginPw());
			form[0] = request_data;
			form[1] = NULL;

			// Reuse m_http
			NetworkManager::releaseConnection(m_http);
			m_http = NetworkManager::createConnection();
			m_http->reuse();
			m_http->setForm(form);
			set_header(m_http, authorize);

			// Send
			char URL[MAX_PATH];

			sprintf(URL, "%s/login/startWithoutInvite", kc.getURL());
			m_http->httpPOST(URL, false);

			// Set values
			m_netapi_phase++;
			m_timestart = 0;			// Reset

			KLBDELETEA(authorize);
			return;
		}
		case 2:
		{
			// Startup OK.
			lua_callback(NETAPIMSG_STARTUP_SUCCESS, status_code, m_pRoot);
			NetworkManager::releaseConnection(m_http);
			kc.setToken(NULL);

			m_http = NULL;
			m_request_type = (-1);
			m_netapi_phase = 0;
			m_nonce = 1;

			return;
		}
	}
}

void CKLBNetAPI::login(int phase, int status_code)
{
	CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();
	switch(phase)
	{
		case 0:
		{
			char request_data[256];
			char country_code[4];
			const char* form[2];
			char* authorize = create_authorize_string(kc.getConsumerKey(), m_nonce, kc.setToken(m_pRoot->child()->child()->getString()));
		
			// Request data
			GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SISO3166CTRYNAME, country_code, 4);
			sprintf(request_data, "request_data={\"country_code\": \"%s\",\"login_key\": \"%s\",\"login_passwd\": \"%s\"}", country_code, kc.getLoginKey(), kc.getLoginPw());
			form[0] = request_data;
			form[1] = NULL;

			// create new HTTP
			NetworkManager::releaseConnection(m_http);
			m_http = NetworkManager::createConnection();
			m_http->reuse();
			m_http->setForm(form);
			set_header(m_http, authorize);

			// send
			char url[MAX_PATH];
			sprintf(url, "%s/login/login", kc.getURL());
			m_http->httpPOST(url, false);

			// Set values
			m_netapi_phase++;
			m_timestart = 0;			// Reset

			KLBDELETEA(authorize);
			return;
		}
		case 1:
		{
			// Login OK
			char user_id[16];
			
			if((status_code = m_pRoot->child()->next()->getInt()) == 200)
			{
				kc.setToken(m_pRoot->child()->child()->getString());	// Authorize token
		
				sprintf(user_id, "%d", m_pRoot->child()->child()->next()->getInt());
				kc.setUserID(user_id);	// User ID
		
				NetworkManager::releaseConnection(m_http);	// Release it first before calling lua callback.
				m_http = NULL;
				m_request_type = (-1);
				m_netapi_phase = 0;

				lua_callback(NETAPIMSG_LOGIN_SUCCESS, status_code, m_pRoot, 1);
			}
			else
			{
				NetworkManager::releaseConnection(m_http);	// Release it first before calling lua callback.
				m_http = NULL;
				m_request_type = (-1);
				m_netapi_phase = 0;

				lua_callback(NETAPIMSG_LOGIN_FAILED, status_code, m_pRoot, 1);
			}

			return;
		}
	}
}

void CKLBNetAPI::request_authkey(int timeout)
{
	CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();
	CKLBHTTPInterface* http = NetworkManager::createConnection();
	http->reuse();

	// Authorize string
	{
		const char* auth = create_authorize_string(kc.getConsumerKey(), m_nonce);
		set_header(http, auth);

		KLBDELETEA(auth);
	}

	// Send
	char url[MAX_PATH];
	sprintf(url, "%s/login/authkey", kc.getURL());

	m_http = http;
	m_timeout = timeout;
	m_timestart = 0;
	http->httpPOST(url, false);
}

void
CKLBNetAPI::execute(u32 deltaT)
{
	if (!m_http) {
		return; // Do nothing if no active connection
	}

	m_timestart += deltaT;

	// Check cancel first
	if (m_canceled) {
		lua_callback(NETAPIMSG_CONNECTION_CANCELED, -1, NULL, m_nonce);

		NetworkManager::releaseConnection(m_http);
		m_http = NULL;
		// Reset flag
		m_canceled = false;
		return;
	}

	// Received data second
	if (m_http->httpRECV() || (m_http->getHttpState() != -1)) {
		// Get Data
		u8* body	= m_http->getRecvResource();
		u32 bodyLen	= body ? m_http->getSize() : 0;
		
		// Get Status Code
		CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();
		int state = m_http->getHttpState();
		bool invalid = ((state >= 400) && (state <= 599)) || (state == 204);
		int msg = invalid == false ? NETAPIMSG_REQUEST_SUCCESS : NETAPIMSG_SERVER_ERROR;

		if(m_http->isMaintenance())
		{
			NetworkManager::releaseConnection(m_http);
			m_http = NULL;

			CKLBLuaEnv::getInstance().intoMaintenance();
			return;
		}

		//
		// Support only JSon for callback
		// 
		freeJSonResult();

		if(bodyLen > 0) m_pRoot = getJsonTree((const char*)body, bodyLen);

		/* Upps, server sends invalid JSON */
		if(m_pRoot == NULL)
		{
			NetworkManager::releaseConnection(m_http);
			m_http = NULL;
			lua_callback(NETAPIMSG_SERVER_ERROR, state, NULL, m_nonce);

			return;
		}
		
		if(invalid == false)
		{
			m_nonce++;	// Increase nonce if request success.

			puts("!====Response Data====!");
			fwrite(body, 1, bodyLen, stdout);
			puts("\n*====Response Data====*");

			if(m_request_type == NETAPI_STARTUP)
				return startUp(m_netapi_phase, state);
			else if(m_request_type == NETAPI_LOGIN)
				return login(m_netapi_phase, state);

			// Check if we're outdated
			if(m_downloading == false)
			{
				const char* server_ver[1];
				if(m_http->hasHeader("Server-Version", server_ver))
				{
					if(strncmp(*server_ver, kc.getClient(), strlen(kc.getClient())))
					{
						// We need to download some data.
						NetworkManager::releaseConnection(m_http);
						m_http = NULL;
						m_request_type = (-1);
						m_downloading = true;
						CKLBScriptEnv::getInstance().call_netAPI_versionUp(m_verup_callback, this, kc.getClient(), *server_ver);

						return;
					}
				}
			}
			fail_times = 0;

			NetworkManager::releaseConnection(m_http);
			m_http = NULL;
			m_request_type = (-1);
			lua_callback(msg, state, m_pRoot, m_nonce - 1);

			return;
		}

		NetworkManager::releaseConnection(m_http);
		m_http = NULL;
		m_request_type = (-1);

		lua_callback(msg, state, m_pRoot, m_nonce);

		return;
	}

	

	if ((m_http->m_threadStop == 1) && (m_http->getHttpState() == -1)) {
		if(fail_times < 5)
		{
			fail_times++;
			goto netapi_timeout;
		}
		lua_callback(map_netapi_fail(m_request_type), -1, NULL, m_nonce);
		NetworkManager::releaseConnection(m_http);
		m_http = NULL;
	}

	// Time out third (after check that valid has arrived)
	if (m_timestart >= m_timeout) {
		netapi_timeout:
		lua_callback(NETAPIMSG_SERVER_TIMEOUT, -1, NULL, m_nonce);
		NetworkManager::releaseConnection(m_http);
		m_http = NULL;
		return;
	}
}

void
CKLBNetAPI::die()
{
	if (m_http) {
		NetworkManager::releaseConnection(m_http);
	}
	KLBDELETEA(m_callback);
	freeHeader();
	freeJSonResult();
}

void
CKLBNetAPI::freeJSonResult() {
	KLBDELETE(m_pRoot);
}

void
CKLBNetAPI::freeHeader() {
	if (m_http_header_array) {
		for (u32 n=0; n < m_http_header_length; n++) {
			KLBDELETEA(m_http_header_array[n]);
		}
		KLBDELETEA(m_http_header_array);
		m_http_header_array = NULL;
	}
}

CKLBNetAPI* 
CKLBNetAPI::create( CKLBTask* pParentTask, 
                    const char * callback) 
{
	CKLBNetAPI* pTask = KLBNEW(CKLBNetAPI);
    if(!pTask) { return NULL; }

	if(!pTask->init(pParentTask, callback)) {
		KLBDELETE(pTask);
		return NULL;
	}
	return pTask;
}

bool 
CKLBNetAPI::init(	CKLBTask* pTask,
					const char * callback) 
{
	m_callback = (callback) ? CKLBUtility::copyString(callback) : NULL;

	// 一通り初期化値が作れたのでタスクを登録
	bool res = regist(pTask, P_INPUT);
	return res;
}

bool
CKLBNetAPI::initScript(CLuaState& lua)
{
	int argc = lua.numArgs();
	lua.print_stack();

    if (argc < 7) { return false; }

	CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();
	kc.setURL(lua.getString(1));
	kc.setConsumernKey(lua.getString(2));
	kc.setClient(lua.getString(3));
	kc.setAppID(lua.getString(4));
	kc.setRegion(lua.getString(7));

	if(lua.isString(8))
		m_verup_callback = CKLBUtility::copyString(lua.getString(8));

	fail_times = 0;

	return init(NULL, lua.getString(5));
}

CKLBJsonItem *
CKLBNetAPI::getJsonTree(const char * json_string, u32 dataLen)
{
	CKLBJsonItem * pRoot = CKLBJsonItem::ReadJsonData((const char *)json_string, dataLen);

	return pRoot;
}

// authorize_string ends with NULL
void CKLBNetAPI::set_header(CKLBHTTPInterface* http, const char* authorize_string)
{
	/*
	API-Model: straightforward
	Application-ID: %s
	Authorize: authorize_string
	Bundle-Version: getBundleVersion()
	Client-Version: %s
	Debug: 1
	OS: Win32
	OS-Version: %s
	Platform-Type: 0 or 3?
	Region: %s
	Time-Zone: %s
	User-ID: %s
	*/

	// Basic vars
	CPFInterface& pfif = CPFInterface::getInstance();
	CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();
	const char* headers[13];
	const char* os_info = pfif.platform().getPlatform();

	// For values above
	char* alldata = new char[1280];
	char* os_data = alldata;
	char* os_version = alldata + 128;
	char* time_zone = alldata + 256;
	char* application_id = time_zone + 128;
	char* authorize = NULL; // Special
	char* bundle_version = time_zone + 256;
	char* client_version = bundle_version + 128;
	char* region = bundle_version + 256;
	char* user_id = region + 128;

	// Process OS and timezone
	{
		char* temp_os_version;
		char* temp_time_zone;
		size_t os_info_len = strlen(os_info);

		memcpy(os_data, os_info, os_info_len + 1);

		for(; *os_data != ';'; os_data++) {}

		*os_data = 0;
		temp_os_version = ++os_data;

		for(; *os_data != ';'; os_data++) {}
		*os_data = 0;
		temp_time_zone = ++os_data;

		sprintf(os_version, "OS-Version: %s", temp_os_version);
		sprintf(time_zone, "Time-Zone: %s", temp_time_zone);
	}

	// Process authorize string
	authorize = new char[1024];

	sprintf(authorize, "Authorize: %s", authorize_string);
	sprintf(application_id, "Application-ID: %s", kc.getAppID());
	sprintf(bundle_version, "Bundle-Version: %s", pfif.platform().getBundleVersion());
	sprintf(client_version, "Client-Version: %s", kc.getClient());
	sprintf(region, "Region: %s", kc.getRegion());

	// User-ID
	{
		const char* uid = kc.getUserID();
		
		if(uid == NULL)
			user_id = NULL;
		else
			sprintf(user_id, "User-ID: %s", uid);
	}

	// Set header
	headers[0] = "API-Model: straightforward";
	headers[1] = application_id;
	headers[2] = authorize;
	headers[3] = bundle_version;
	headers[4] = client_version;
	headers[5] = "Debug: 1";
	headers[6] = "OS: Win32";
	headers[7] = os_version;
	headers[8] = SIF_Win32::AndroidMode ? "Platform-Type: 2" : "Platform-Type: 3";
	headers[9] = region;
	headers[10] = time_zone;
	headers[11] = user_id;
	headers[12] = NULL;

	http->setHeader(headers);

	delete[] authorize;
	delete[] alldata;
}

int
CKLBNetAPI::commandScript(CLuaState& lua)
{
	int argc = lua.numArgs();

	if(argc < 2) {
		lua.retBoolean(false);
		return 1;
	}
	lua.print_stack();
	int cmd = m_request_type = lua.getInt(2);
	int ret = 1;

	switch(cmd)
	{
		default:
		{
			lua.retBoolean(false);
		}
		break;
		case NETAPI_STARTUP:
		{
			//
			// 3. login_key
			// 4. login_passwd
			// 5. invite
			// 6. timeout
			//
			if(argc < 4) lua.retBoolean(false);
			else {
				const char* user_id = lua.getString(3);
				const char* password = lua.getString(4);
				int timeout = 30000;

				if(lua.isNum(6))
					timeout = lua.getInt(6);

				// Save credentials
				CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();
				kc.setLoginKey(user_id);
				kc.setLoginPwd(password);
				
				// Create HTTP
				m_timeout = timeout;
				m_timestart = 0;
				request_authkey(timeout);

				lua.retBoolean(true);
			}
		}
		break;
		case NETAPI_LOGIN:
		{
			//
			// 3. login_key
			// 4. login_passwd
			// 5. timeout
			//
			CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();
			const char* auth;

			kc.setLoginKey(lua.getString(3));
			kc.setLoginPwd(lua.getString(4));

			auth = create_authorize_string(kc.getConsumerKey(), m_nonce);

			m_timeout = lua.getInt(5);
			m_timestart = 0;
			
			request_authkey(m_timeout);

			lua.retInt(m_nonce);
		}
		break;
		case NETAPI_CANCEL:
		case NETAPI_CANCEL_ALL:
		{
			if (m_http != NULL) {
				m_canceled = true;
			}
			lua.retBoolean(m_http != NULL);
		}
		break;
		case NETAPI_SEND:
		{
			//
			// 3. Request data table
			// 4. End point URL. Defaults to "/api" if is nil
			// 5. Timeout
			// 6. Skip version check?
			//
			CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();
			if(argc < 3 || argc > 6) {
				lua.retBoolean(false);
			}
			else {
				char api[MAX_PATH];
				const char* end_point = "/api";
				const char* url_target = NULL;

				if(lua.isString(4))
					end_point = url_target = lua.getString(4);

				if(url_target != NULL && strstr(url_target, "http://") == url_target)
					memcpy(api, url_target, strlen(url_target));
				else
					sprintf(api, "%s%s", kc.getURL(), end_point);

				// Header list
				const char** headers = NULL;
				const char** values  = NULL;
				freeHeader();

				// POST JSon payload
				u32 send_json_size = 0;
				const char* send_json = NULL;

				m_http = NetworkManager::createConnection();

				if (m_http) {

					lua.retValue(3);
					send_json = CKLBUtility::lua2json(lua, send_json_size);
					lua.pop(1);
					
					if (send_json) {
						char* json;
						const char * items[2];
						const char* req = "request_data=";
						const char* authorize = create_authorize_string(kc.getConsumerKey(), m_nonce, kc.getToken());
						int send_json_length = strlen( send_json );
						int req_length = strlen( req );

						if(lua.isBool(6) == false || lua.getBool(6) == false)
							m_downloading = false;

						json = KLBNEWA( char , send_json_length+req_length+1 );
						strcpy( json , req );
						strcat( json , send_json );
						items[0] = json;
						items[1] = NULL;

						set_header(m_http, authorize);
						m_http->setForm(items);
						m_http->httpPOST(api, false);

						KLBDELETEA(json);
						KLBDELETEA(authorize);
					} else {
						m_http->httpGET(api, false);
					}

					m_timeout	= lua.getInt(5);
					m_timestart = 0;

					lua.retInt(m_nonce);
				} else {
					// Connection creation failed.
					lua.retBoolean(false);
				}
			}
		}
		break;
	}
	return 1;
}

bool
CKLBNetAPI::lua_callback(int msg, int status, CKLBJsonItem * pRoot, int uniq)
{
	return CKLBScriptEnv::getInstance().call_netAPI_callback(m_callback, this, uniq, msg, status, pRoot);
}
