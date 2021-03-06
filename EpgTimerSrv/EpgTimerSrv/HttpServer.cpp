#include "StdAfx.h"
#include "HttpServer.h"
#include "../../Common/StringUtil.h"
#include "../../Common/PathUtil.h"
#include "../../Common/ParseTextInstances.h"
#include "civetweb.h"
#include <io.h>
#include <fcntl.h>

#define LUA_DLL_NAME L"lua52.dll"

namespace
{
const char UPNP_URN_DMS_1[] = "urn:schemas-upnp-org:device:MediaServer:1";
const char UPNP_URN_CDS_1[] = "urn:schemas-upnp-org:service:ContentDirectory:1";
const char UPNP_URN_CMS_1[] = "urn:schemas-upnp-org:service:ConnectionManager:1";
const char UPNP_URN_AVT_1[] = "urn:schemas-upnp-org:service:AVTransport:1";
}

CHttpServer::CHttpServer()
	: mgContext(NULL)
	, hLuaDll(NULL)
{
}

CHttpServer::~CHttpServer()
{
	StopServer();
}

bool CHttpServer::StartServer(const SERVER_OPTIONS& op, const std::function<void(lua_State*)>& initProc)
{
	StopServer();

	//LuaのDLLが無いとき分かりにくいタイミングでエラーになるので事前に読んでおく(必須ではない)
	this->hLuaDll = LoadLibrary(LUA_DLL_NAME);
	if( this->hLuaDll == NULL ){
		OutputDebugString(L"CHttpServer::StartServer(): " LUA_DLL_NAME L" not found.\r\n");
		return false;
	}
	string ports;
	WtoUTF8(op.ports, ports);
	string rootPathU;
	WtoUTF8(op.rootPath, rootPathU);
	//パスにASCII範囲外を含むのは(主にLuaが原因で)難ありなので蹴る
	for( size_t i = 0; i < rootPathU.size(); i++ ){
		if( rootPathU[i] & 0x80 ){
			OutputDebugString(L"CHttpServer::StartServer(): path has multibyte.\r\n");
			return false;
		}
	}
	string accessLogPath;
	//ログは_wfopen()されるのでWtoUTF8()。civetweb.cのACCESS_LOG_FILEとERROR_LOG_FILEの扱いに注意
	WtoUTF8(GetModulePath().replace_filename(L"HttpAccess.log").native(), accessLogPath);
	string errorLogPath;
	WtoUTF8(GetModulePath().replace_filename(L"HttpError.log").native(), errorLogPath);
	string sslCertPath;
	//認証鍵は実質fopen()されるのでWtoA()
	WtoA(GetModulePath().replace_filename(L"ssl_cert.pem").native(), sslCertPath);
	string sslPeerPath;
	WtoA(GetModulePath().replace_filename(L"ssl_peer.pem").native(), sslPeerPath);
	string globalAuthPath;
	//グローバルパスワードは_wfopen()されるのでWtoUTF8()
	WtoUTF8(GetModulePath().replace_filename(L"glpasswd").native(), globalAuthPath);

	//Access Control List
	string acl;
	WtoUTF8(op.accessControlList, acl);
	acl = "-0.0.0.0/0," + acl;

	string authDomain;
	WtoUTF8(op.authenticationDomain, authDomain);
	string sslCipherList;
	WtoUTF8(op.sslCipherList, sslCipherList);
	string numThreads;
	Format(numThreads, "%d", min(max(op.numThreads, 1), 50));
	string requestTimeout;
	Format(requestTimeout, "%d", max(op.requestTimeout, 1));
	string sslProtocolVersion;
	Format(sslProtocolVersion, "%d", op.sslProtocolVersion);

	//追加のMIMEタイプ
	CParseContentTypeText contentType;
	contentType.ParseText(GetModulePath().replace_filename(L"ContentTypeText.txt").c_str());
	wstring extraMimeW;
	for( map<wstring, wstring>::const_iterator itr = contentType.GetMap().begin(); itr != contentType.GetMap().end(); itr++ ){
		extraMimeW += itr->first + L'=' + itr->second + L',';
	}
	string extraMime;
	WtoUTF8(extraMimeW, extraMime);

	const char* options[64] = {
		"ssi_pattern", "",
		"enable_keep_alive", op.keepAlive ? "yes" : "no",
		"access_control_list", acl.c_str(),
		"extra_mime_types", extraMime.c_str(),
		"listening_ports", ports.c_str(),
		"document_root", rootPathU.c_str(),
		"num_threads", numThreads.c_str(),
		"request_timeout_ms", requestTimeout.c_str(),
		"ssl_ca_file", sslPeerPath.c_str(),
		"ssl_default_verify_paths", "no",
		"ssl_cipher_list", sslCipherList.c_str(),
		"ssl_protocol_version", sslProtocolVersion.c_str(),
		"lua_script_pattern", "**.lua$|**.html$|*/api/*$",
	};
	int opCount = 2 * 13;
	if( op.saveLog ){
		options[opCount++] = "access_log_file";
		options[opCount++] = accessLogPath.c_str();
		options[opCount++] = "error_log_file";
		options[opCount++] = errorLogPath.c_str();
	}
	if( authDomain.empty() == false ){
		options[opCount++] = "authentication_domain";
		options[opCount++] = authDomain.c_str();
	}
	if( ports.find('s') != string::npos ){
		//セキュアポートを含むので認証鍵を指定する
		options[opCount++] = "ssl_certificate";
		options[opCount++] = sslCertPath.c_str();
	}
	wstring sslPeerPathW;
	AtoW(sslPeerPath, sslPeerPathW);
	if( GetFileAttributes(sslPeerPathW.c_str()) != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND ){
		//信頼済み証明書ファイルが「存在しないことを確信」できなければ有効にする
		options[opCount++] = "ssl_verify_peer";
		options[opCount++] = "yes";
	}
	wstring globalAuthPathW;
	UTF8toW(globalAuthPath, globalAuthPathW);
	if( GetFileAttributes(globalAuthPathW.c_str()) != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND ){
		//グローバルパスワードは「存在しないことを確信」できなければ指定しておく
		options[opCount++] = "global_auth_file";
		options[opCount++] = globalAuthPath.c_str();
	}

	this->initLuaProc = initProc;
	mg_callbacks callbacks = {};
	callbacks.init_lua = &InitLua;
	this->mgContext = mg_start(&callbacks, this, options);

	if( this->mgContext && op.enableSsdpServer ){
		//"<UDN>uuid:{UUID}</UDN>"が必要
		string notifyUuid;
		std::unique_ptr<FILE, decltype(&fclose)> fp(shared_wfopen(fs_path(op.rootPath).append(L"dlna\\dms\\ddd.xml").c_str(), L"rbN"), fclose);
		if( fp ){
			char olbuff[257];
			for( size_t n = fread(olbuff, 1, 256, fp.get()); ; n = fread(olbuff + 64, 1, 192, fp.get()) + 64 ){
				olbuff[n] = '\0';
				char* udn = strstr(olbuff, "<UDN>uuid:");
				if( udn && strlen(udn) >= 10 + 36 + 6 && strncmp(udn + 10 + 36, "</UDN>", 6) == 0 ){
					notifyUuid.assign(udn + 5, 41);
					break;
				}
				if( n < 256 ){
					break;
				}
				memcpy(olbuff, olbuff + 192, 64);
			}
		}
		if( notifyUuid.empty() == false ){
			//最後にみつかった':'より後ろか先頭をatoiした結果を通知ポートとする
			int notifyPort = atoi(ports.c_str() + (ports.find_last_of(':') == string::npos ? 0 : ports.find_last_of(':') + 1)) & 0xFFFF;
			//UPnPのUDP(Port1900)部分を担当するサーバ
			LPCSTR targetArray[] = { "upnp:rootdevice", UPNP_URN_DMS_1, UPNP_URN_CDS_1, UPNP_URN_CMS_1, UPNP_URN_AVT_1 };
			vector<CUpnpSsdpServer::SSDP_TARGET_INFO> targetList(2 + _countof(targetArray));
			targetList[0].target = notifyUuid;
			Format(targetList[0].location, "http://$HOST$:%d/dlna/dms/ddd.xml", notifyPort);
			targetList[0].usn = targetList[0].target;
			targetList[0].notifyFlag = true;
			targetList[1].target = "ssdp:all";
			targetList[1].location = targetList[0].location;
			targetList[1].usn = notifyUuid + "::" + "upnp:rootdevice";
			targetList[1].notifyFlag = false;
			for( size_t i = 2; i < targetList.size(); i++ ){
				targetList[i].target = targetArray[i - 2];
				targetList[i].location = targetList[0].location;
				targetList[i].usn = notifyUuid + "::" + targetList[i].target;
				targetList[i].notifyFlag = true;
			}
			this->upnpSsdpServer.Start(targetList);
		}else{
			OutputDebugString(L"CHttpServer::StartServer(): invalid /dlna/dms/ddd.xml\r\n");
		}
	}
	return this->mgContext != NULL;
}

bool CHttpServer::StopServer(bool checkOnly)
{
	if( this->mgContext ){
		this->upnpSsdpServer.Stop();
		if( checkOnly ){
			if( mg_check_stop(this->mgContext) == 0 ){
				return false;
			}
		}else{
			//正常であればmg_stop()はreqToを超えて待機することはない
			DWORD reqTo = atoi(mg_get_option(this->mgContext, "request_timeout_ms"));
			DWORD tick = GetTickCount();
			while( GetTickCount() - tick < reqTo + 10000 ){
				if( mg_check_stop(this->mgContext) ){
					this->mgContext = NULL;
					break;
				}
				Sleep(10);
			}
			if( this->mgContext ){
				OutputDebugString(L"CHttpServer::StopServer(): failed to stop service.\r\n");
			}
		}
		this->mgContext = NULL;
	}
	if( this->hLuaDll ){
		FreeLibrary(this->hLuaDll);
		this->hLuaDll = NULL;
	}
	return true;
}

void CHttpServer::InitLua(const mg_connection* conn, void* luaContext)
{
	const CHttpServer* sys = (CHttpServer*)mg_get_user_data(mg_get_context(conn));
	lua_State* L = (lua_State*)luaContext;
	sys->initLuaProc(L);
}

namespace LuaHelp
{

#if 1 //Refer: civetweb/mod_lua.inl
void reg_string_(lua_State* L, const char* name, size_t size, const char* val)
{
	lua_pushlstring(L, name, size - 1);
	lua_pushstring(L, val);
	lua_rawset(L, -3);
}

void reg_int_(lua_State* L, const char* name, size_t size, int val)
{
	lua_pushlstring(L, name, size - 1);
	lua_pushinteger(L, val);
	lua_rawset(L, -3);
}

void reg_boolean_(lua_State* L, const char* name, size_t size, bool val)
{
	lua_pushlstring(L, name, size - 1);
	lua_pushboolean(L, val);
	lua_rawset(L, -3);
}
#endif

void reg_time_(lua_State* L, const char* name, size_t size, const SYSTEMTIME& st)
{
	lua_pushlstring(L, name, size - 1);
	lua_createtable(L, 0, 9);
	reg_int(L, "year", st.wYear);
	reg_int(L, "month", st.wMonth);
	reg_int(L, "day", st.wDay);
	reg_int(L, "hour", st.wHour);
	reg_int(L, "min", st.wMinute);
	reg_int(L, "sec", st.wSecond);
	reg_int(L, "msec", st.wMilliseconds);
	reg_int(L, "wday", st.wDayOfWeek + 1);
	reg_boolean(L, "isdst", 0);
	lua_rawset(L, -3);
}

bool isnil(lua_State* L, const char* name)
{
	lua_getfield(L, -1, name);
	bool ret = lua_isnil(L, -1);
	lua_pop(L, 1);
	return ret;
}

string get_string(lua_State* L, const char* name)
{
	lua_getfield(L, -1, name);
	const char* p = lua_tostring(L, -1);
	string ret = p ? p : "";
	lua_pop(L, 1);
	return ret;
}

int get_int(lua_State* L, const char* name)
{
	lua_getfield(L, -1, name);
	int ret = (int)lua_tointeger(L, -1);
	lua_pop(L, 1);
	return ret;
}

bool get_boolean(lua_State* L, const char* name)
{
	lua_getfield(L, -1, name);
	bool ret = lua_toboolean(L, -1) != 0;
	lua_pop(L, 1);
	return ret;
}

SYSTEMTIME get_time(lua_State* L, const char* name)
{
	SYSTEMTIME ret = {};
	lua_getfield(L, -1, name);
	if( lua_istable(L, -1) ){
		SYSTEMTIME st;
		st.wYear = (WCHAR)get_int(L, "year");
		st.wMonth = (WCHAR)get_int(L, "month");
		st.wDay = (WCHAR)get_int(L, "day");
		st.wHour = (WCHAR)get_int(L, "hour");
		st.wMinute = (WCHAR)get_int(L, "min");
		st.wSecond = (WCHAR)get_int(L, "sec");
		st.wMilliseconds = (WCHAR)get_int(L, "msec");
		FILETIME ft;
		if( SystemTimeToFileTime(&st, &ft) && FileTimeToSystemTime(&ft, &st) ){
			ret = st;
		}
	}
	lua_pop(L, 1);
	return ret;
}

namespace
{

wchar_t* utf8towcsdup(const char* s, const wchar_t* prefix = L"")
{
	int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
	if( len > 0 ){
		wchar_t* w = (wchar_t*)calloc(wcslen(prefix) + len, sizeof(wchar_t));
		if( w != NULL ){
			wcscpy_s(w, wcslen(prefix) + 1, prefix);
			if( MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w + wcslen(prefix), len) > 0 ){
				return w;
			}
		}
		free(w);
	}
	return NULL;
}

void nefree(void* p)
{
	int en = errno;
	free(p);
	errno = en;
}

struct LStream {
	FILE* f; //stream (NULL for incompletely created streams)
	lua_CFunction closef; //to close stream (NULL for closed streams)
	HANDLE procf; //process handle used by f_pclose
};

//Refer: Lua-5.2.4/liolib.c (See Copyright Notice in lua.h)

int checkmode(const char* mode)
{
	return *mode != '\0' && strchr("rwa", *(mode++)) != NULL &&
		(*mode != '+' || ++mode) && //skip if char is '+'
		(*mode != 'b' || ++mode) && //skip if char is 'b'
		(*mode == '\0');
}

LStream* tolstream(lua_State* L)
{
	return (LStream*)luaL_checkudata(L, 1, "EDCB_FILE*");
}

int f_tostring(lua_State* L)
{
	LStream* p = tolstream(L);
	if( p->closef == NULL )
		lua_pushliteral(L, "edcb_file (closed)");
	else
		lua_pushfstring(L, "edcb_file (%p)", p->f);
	return 1;
}

FILE* tofile(lua_State* L)
{
	LStream* p = tolstream(L);
	if( p->closef == NULL )
		luaL_error(L, "attempt to use a closed file");
	lua_assert(p->f);
	return p->f;
}

LStream* newprefile(lua_State* L)
{
	LStream* p = (LStream*)lua_newuserdata(L, sizeof(LStream));
	p->closef = NULL; //mark file handle as 'closed'
	luaL_setmetatable(L, "EDCB_FILE*");
	return p;
}

int aux_close(lua_State* L)
{
	LStream* p = tolstream(L);
	lua_CFunction cf = p->closef;
	p->closef = NULL; //mark stream as closed
	return (*cf)(L); //close it
}

int f_close(lua_State* L)
{
	tofile(L); //make sure argument is an open stream
	return aux_close(L);
}

int f_gc(lua_State* L)
{
	LStream* p = tolstream(L);
	if( p->closef != NULL && p->f != NULL )
		aux_close(L); //ignore closed and incompletely open files
	return 0;
}

int f_fclose(lua_State* L)
{
	LStream* p = tolstream(L);
	return luaL_fileresult(L, (fclose(p->f) == 0), NULL);
}

int f_pclose(lua_State* L)
{
	LStream* p = tolstream(L);
	fclose(p->f);
	DWORD dw;
	int stat = -1;
	if( WaitForSingleObject(p->procf, INFINITE) == WAIT_OBJECT_0 && GetExitCodeProcess(p->procf, &dw) ){
		stat = (int)dw;
	}
	CloseHandle(p->procf);
	errno = ECHILD;
	return luaL_execresult(L, stat);
}

int test_eof(lua_State* L, FILE* f)
{
	int c = getc(f);
	ungetc(c, f);
	lua_pushlstring(L, NULL, 0);
	return (c != EOF);
}

void read_all(lua_State* L, FILE* f)
{
	size_t rlen = LUAL_BUFFERSIZE; //how much to read in each cycle
	luaL_Buffer b;
	luaL_buffinit(L, &b);
	for(;;){
		char* p = luaL_prepbuffsize(&b, rlen);
		size_t nr = fread(p, sizeof(char), rlen, f);
		luaL_addsize(&b, nr);
		if( nr < rlen ) break; //eof?
		else if( rlen <= ((~(size_t)0) / 4) ) //avoid buffers too large
			rlen *= 2; //double buffer size at each iteration
	}
	luaL_pushresult(&b); //close buffer
}

int read_chars(lua_State* L, FILE* f, size_t n)
{
	luaL_Buffer b;
	luaL_buffinit(L, &b);
	char* p = luaL_prepbuffsize(&b, n); //prepare buffer to read whole block
	size_t nr = fread(p, sizeof(char), n, f); //try to read 'n' chars
	luaL_addsize(&b, nr);
	luaL_pushresult(&b); //close buffer
	return (nr > 0); //true iff read something
}

int f_read(lua_State* L)
{
	FILE* f = tofile(L);
	int nargs = lua_gettop(L) - 1;
	int success;
	int n;
	clearerr(f);
	if( nargs == 0 ){ //no arguments?
		return luaL_error(L, "invalid format");
	}else{ //ensure stack space for all results and for auxlib's buffer
		luaL_checkstack(L, nargs + LUA_MINSTACK, "too many arguments");
		success = 1;
		for( n = 2; nargs-- && success; n++ ){
			if( lua_type(L, n) == LUA_TNUMBER ){
				size_t l = (size_t)lua_tointeger(L, n);
				success = (l == 0) ? test_eof(L, f) : read_chars(L, f, l);
			}else{
				const char* p = lua_tostring(L, n);
				luaL_argcheck(L, p && p[0] == '*', n, "invalid option");
				switch( p[1] ){
				case 'a': //file
					read_all(L, f); //read entire file
					success = 1; //always success
					break;
				default:
					return luaL_argerror(L, n, "invalid format");
				}
			}
		}
	}
	if( ferror(f) )
		return luaL_fileresult(L, 0, NULL);
	if( !success ){
		lua_pop(L, 1); //remove last result
		lua_pushnil(L); //push nil instead
	}
	return n - 2;
}

int f_write(lua_State* L)
{
	FILE* f = tofile(L);
	lua_pushvalue(L, 1); //push file at the stack top (to be returned)
	int arg = 2;
	int nargs = lua_gettop(L) - arg;
	int status = 1;
	for( ; nargs--; arg++ ){
		size_t l;
		const char* s = luaL_checklstring(L, arg, &l);
		status = status && (fwrite(s, sizeof(char), l, f) == l);
	}
	if( status ) return 1; //file handle already on stack top
	else return luaL_fileresult(L, status, NULL);
}

int f_seek(lua_State* L)
{
	static const int mode[] = { SEEK_SET, SEEK_CUR, SEEK_END };
	static const char* const modenames[] = { "set", "cur", "end", NULL };
	FILE* f = tofile(L);
	int op = luaL_checkoption(L, 2, "cur", modenames);
	lua_Number p3 = luaL_optnumber(L, 3, 0);
	__int64 offset = (__int64)p3;
	luaL_argcheck(L, (lua_Number)offset == p3, 3, "not an integer in proper range");
	op = _fseeki64(f, offset, mode[op]);
	if( op )
		return luaL_fileresult(L, 0, NULL); //error
	else{
		lua_pushnumber(L, (lua_Number)_ftelli64(f));
		return 1;
	}
}

int f_setvbuf(lua_State* L)
{
	static const int mode[] = { _IONBF, _IOFBF, _IOLBF };
	static const char* const modenames[] = { "no", "full", "line", NULL };
	FILE* f = tofile(L);
	int op = luaL_checkoption(L, 2, NULL, modenames);
	lua_Integer sz = luaL_optinteger(L, 3, LUAL_BUFFERSIZE);
	int res = setvbuf(f, NULL, mode[op], sz);
	return luaL_fileresult(L, res == 0, NULL);
}

int f_flush(lua_State* L)
{
	return luaL_fileresult(L, fflush(tofile(L)) == 0, NULL);
}

}

//Refer: Lua-5.2.4/loslib.c (See Copyright Notice in lua.h)

int os_execute(lua_State* L)
{
	wchar_t cmdexe[MAX_PATH];
	DWORD dw = GetEnvironmentVariable(L"ComSpec", cmdexe, MAX_PATH);
	if( dw == 0 || dw >= MAX_PATH ){
		cmdexe[0] = L'\0';
	}
	const char* cmd = luaL_optstring(L, 1, NULL);
	if( cmd != NULL ){
		DWORD creflags = lua_toboolean(L, 2) ? 0 : CREATE_NO_WINDOW;
		wchar_t* wcmd = utf8towcsdup(cmd, L" /c ");
		luaL_argcheck(L, wcmd != NULL, 1, "utf8towcsdup");
		STARTUPINFO si = {};
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi;
		int stat = -1;
		if( cmdexe[0] && CreateProcess(cmdexe, wcmd, NULL, NULL, FALSE, creflags, NULL, NULL, &si, &pi) ){
			CloseHandle(pi.hThread);
			if( WaitForSingleObject(pi.hProcess, INFINITE) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &dw) ){
				stat = (int)dw;
			}
			CloseHandle(pi.hProcess);
		}
		errno = ENOENT;
		nefree(wcmd);
		return luaL_execresult(L, stat);
	}else{
		lua_pushboolean(L, cmdexe[0]); //true if there is a shell
		return 1;
	}
}

int os_remove(lua_State* L)
{
	const char* filename = luaL_checkstring(L, 1);
	wchar_t* wfilename = utf8towcsdup(filename);
	luaL_argcheck(L, wfilename != NULL, 1, "utf8towcsdup");
	int stat = _wremove(wfilename);
	nefree(wfilename);
	return luaL_fileresult(L, stat == 0, filename);
}

int os_rename(lua_State* L)
{
	const char* fromname = luaL_checkstring(L, 1);
	const char* toname = luaL_checkstring(L, 2);
	wchar_t* wfromname = utf8towcsdup(fromname);
	luaL_argcheck(L, wfromname != NULL, 1, "utf8towcsdup");
	wchar_t* wtoname = utf8towcsdup(toname);
	if( wtoname == NULL ){
		free(wfromname);
		luaL_argerror(L, 2, "utf8towcsdup");
	}
	int stat = _wrename(wfromname, wtoname);
	nefree(wtoname);
	nefree(wfromname);
	return luaL_fileresult(L, stat == 0, NULL);
}

//Refer: Lua-5.2.4/liolib.c (See Copyright Notice in lua.h)

int io_open(lua_State* L)
{
	const char* filename = luaL_checkstring(L, 1);
	const char* mode = luaL_optstring(L, 2, "r");
	LStream* p = newprefile(L);
	p->f = NULL;
	p->closef = &f_fclose;
	luaL_argcheck(L, checkmode(mode), 2, "invalid mode");
	wchar_t* wfilename = utf8towcsdup(filename);
	luaL_argcheck(L, wfilename != NULL, 1, "utf8towcsdup");
	wchar_t* wmode = utf8towcsdup(mode);
	if( wmode == NULL ){
		free(wfilename);
		luaL_argerror(L, 2, "utf8towcsdup");
	}
	p->f = shared_wfopen(wfilename, wmode);
	nefree(wmode);
	nefree(wfilename);
	return (p->f == NULL) ? luaL_fileresult(L, 0, filename) : 1;
}

int io_popen(lua_State* L)
{
	const char* filename = luaL_checkstring(L, 1);
	const char* mode = luaL_optstring(L, 2, "r");
	DWORD creflags = lua_toboolean(L, 3) ? 0 : CREATE_NO_WINDOW;
	LStream* p = newprefile(L);
	luaL_argcheck(L, (mode[0] == 'r' || mode[0] == 'w') && (!mode[1] || mode[1] == 'b' && !mode[2]), 2, "invalid mode");
	wchar_t* wfilename = utf8towcsdup(filename, L" /c ");
	luaL_argcheck(L, wfilename != NULL, 1, "utf8towcsdup");
	p->f = NULL;
	wchar_t cmdexe[MAX_PATH];
	DWORD dw = GetEnvironmentVariable(L"ComSpec", cmdexe, MAX_PATH);
	if( dw == 0 || dw >= MAX_PATH ){
		cmdexe[0] = L'\0';
	}
	HANDLE ppipe, tpipe, cpipe; //parent, temporary, child
	if( cmdexe[0] && CreatePipe(mode[0] == 'r' ? &ppipe : &tpipe, mode[0] == 'r' ? &tpipe : &ppipe, NULL, 0) ){
		BOOL b = DuplicateHandle(GetCurrentProcess(), tpipe, GetCurrentProcess(), &cpipe, 0, TRUE, DUPLICATE_SAME_ACCESS);
		CloseHandle(tpipe);
		if( b ){
			SECURITY_ATTRIBUTES sa = {};
			sa.nLength = sizeof(sa);
			sa.bInheritHandle = TRUE;
			STARTUPINFO si = {};
			si.cb = sizeof(si);
			si.dwFlags = STARTF_USESTDHANDLES;
			if( mode[0] == 'r' ){
				si.hStdInput = CreateFile(L"nul", GENERIC_READ, 0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
				si.hStdOutput = cpipe;
			}else{
				si.hStdInput = cpipe;
				si.hStdOutput = CreateFile(L"nul", GENERIC_WRITE, 0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			}
			si.hStdError = CreateFile(L"nul", GENERIC_WRITE, 0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			PROCESS_INFORMATION pi;
			b = CreateProcess(cmdexe, wfilename, NULL, NULL, TRUE, creflags, NULL, NULL, &si, &pi);
			if( si.hStdError != INVALID_HANDLE_VALUE ){
				CloseHandle(si.hStdError);
			}
			if( si.hStdOutput != INVALID_HANDLE_VALUE ){
				CloseHandle(si.hStdOutput);
			}
			if( si.hStdInput != INVALID_HANDLE_VALUE ){
				CloseHandle(si.hStdInput);
			}
			if( b ){
				CloseHandle(pi.hThread);
				int osfd = _open_osfhandle((intptr_t)ppipe, mode[1] ? 0 : _O_TEXT);
				if( osfd != -1 ){
					ppipe = INVALID_HANDLE_VALUE;
					p->f = _wfdopen(osfd, mode[0] == 'r' ? (mode[1] ? L"rb" : L"r") : (mode[1] ? L"wb" : L"w"));
					if( p->f ){
						p->procf = pi.hProcess;
					}else{
						_close(osfd);
						CloseHandle(pi.hProcess);
					}
				}else{
					CloseHandle(pi.hProcess);
				}
			}
		}
		if( ppipe != INVALID_HANDLE_VALUE ){
			CloseHandle(ppipe);
		}
	}
	errno = ENOENT;
	nefree(wfilename);
	p->closef = &f_pclose;
	return (p->f == NULL) ? luaL_fileresult(L, 0, filename) : 1;
}

void f_createmeta(lua_State* L)
{
	static const luaL_Reg flib[] = {
		{ "close", f_close },
		{ "flush", f_flush },
		{ "read", f_read },
		{ "seek", f_seek },
		{ "setvbuf", f_setvbuf },
		{ "write", f_write },
		{ "__gc", f_gc },
		{ "__tostring", f_tostring },
		{ NULL, NULL }
	};
	luaL_newmetatable(L, "EDCB_FILE*"); //create metatable for file handles
	lua_pushvalue(L, -1); //push metatable
	lua_setfield(L, -2, "__index"); //metatable.__index = metatable
	luaL_setfuncs(L, flib, 0); //add file methods to new metatable
	lua_pop(L, 1); //pop new metatable
}

}
