/**
  * Touhou Community Reliant Automatic Patcher
  * Main DLL
  *
  * ----
  *
  * DLL and engine initialization.
  */

#include "thcrap.h"
#include "sha256.h"
#include "win32_detour.h"

/// Static global variables
/// -----------------------
// Required to get the exported functions of thcrap.dll.
static HMODULE hThcrap = NULL;
static char *dll_dir = NULL;
/// -----------------------

json_t* identify_by_hash(const char *fn, size_t *file_size, json_t *versions)
{
	unsigned char *file_buffer = file_read(fn, file_size);
	SHA256_CTX sha256_ctx;
	BYTE hash[32];
	char hash_str[65];
	int i;

	if(!file_buffer) {
		return NULL;
	}
	sha256_init(&sha256_ctx);
	sha256_update(&sha256_ctx, file_buffer, *file_size);
	sha256_final(&sha256_ctx, hash);
	SAFE_FREE(file_buffer);

	for(i = 0; i < 32; i++) {
		sprintf(hash_str + (i * 2), "%02x", hash[i]);
	}
	return json_object_get(json_object_get(versions, "hashes"), hash_str);
}

json_t* identify_by_size(size_t file_size, json_t *versions)
{
	return json_object_numkey_get(json_object_get(versions, "sizes"), file_size);
}

json_t* stack_cfg_resolve(const char *fn, size_t *file_size)
{
	json_t *ret = NULL;
	json_t *chain = resolve_chain(fn);
	if(json_array_size(chain)) {
		json_array_insert_new(chain, 0, json_string("global.js"));
		log_printf("(JSON) Resolving configuration for %s... ", fn);
		ret = stack_json_resolve_chain(chain, file_size);
	}
	json_decref(chain);
	return ret;
}

json_t* identify(const char *exe_fn)
{
	size_t exe_size;
	json_t *run_ver = NULL;
	json_t *versions_js = stack_json_resolve("versions.js", NULL);
	json_t *game_obj = NULL;
	json_t *build_obj = NULL;
	json_t *variety_obj = NULL;
	json_t *codepage_obj = NULL;
	const char *game = NULL;
	const char *build = NULL;
	const char *variety = NULL;
	UINT codepage;

	// Result of the EXE identification (array)
	json_t *id_array = NULL;
	int size_cmp = 0;

	if(!versions_js) {
		goto end;
	}
	log_printf("Hashing executable... ");

	id_array = identify_by_hash(exe_fn, &exe_size, versions_js);
	if(!id_array) {
		size_cmp = 1;
		log_printf("failed!\n");
		log_printf("File size lookup... ");
		id_array = identify_by_size(exe_size, versions_js);

		if(!id_array) {
			log_printf("failed!\n");
			goto end;
		}
	}

	game_obj = json_array_get(id_array, 0);
	build_obj = json_array_get(id_array, 1);
	variety_obj = json_array_get(id_array, 2);
	codepage_obj = json_array_get(id_array, 3);
	game = json_string_value(game_obj);
	build = json_string_value(build_obj);
	variety = json_string_value(variety_obj);
	codepage = json_hex_value(codepage_obj);

	if(!game || !build) {
		log_printf("Invalid version format!");
		goto end;
	}

	if(codepage) {
		w32u8_set_fallback_codepage(codepage);
	}

	// Store build in the runconfig to be recalled later for
	// version-dependent patch file resolving. Needs be directly written to
	// run_cfg because we already require it down below to resolve ver_fn.
	json_object_set(run_cfg, "build", build_obj);

	log_printf("→ %s %s %s (codepage %d)\n", game, build, variety, codepage);

	if(stricmp(PathFindExtensionA(game), ".js")) {
		size_t ver_fn_len = json_string_length(game_obj) + 1 + strlen(".js") + 1;
		VLA(char, ver_fn, ver_fn_len);
		sprintf(ver_fn, "%s.js", game);
		run_ver = stack_cfg_resolve(ver_fn, NULL);
		VLA_FREE(ver_fn);
	} else {
		run_ver = stack_cfg_resolve(game, NULL);
	}

	// Ensure that we have a configuration with a "game" key
	if(!run_ver) {
		run_ver = json_object();
	}
	if(!json_object_get_string(run_ver, "game")) {
		json_object_set(run_ver, "game", game_obj);
	}

	if(size_cmp) {
		const char *game_title = json_object_get_string(run_ver, "title");
		int ret;
		if(game_title) {
			game = game_title;
		}
		ret = log_mboxf("Unknown version detected", MB_YESNO | MB_ICONQUESTION,
			"You have attached %s to an unknown game version.\n"
			"According to the file size, this is most likely\n"
			"\n"
			"\t%s %s %s\n"
			"\n"
			"but we haven't tested this exact variety yet and thus can't confirm that the patches will work.\n"
			"They might crash the game, damage your save files or cause even worse problems.\n"
			"\n"
			"Please post <%s> in one of the following places:\n"
			"\n"
			"• Gitter: https://gitter.im/thpatch/thcrap. Requires a GitHub or Twitter account.\n"
			"• IRC: #thcrap on irc.freenode.net. Webchat at https://webchat.freenode.net/?channels=#thcrap\n"
			"\n"
			"We will take a look at it, and add support if possible.\n"
			"\n"
			"Apply patches for the identified game version regardless (on your own risk)?",
			PROJECT_NAME_SHORT(), game, build, variety, exe_fn
		);
		if(ret == IDNO) {
			run_ver = json_decref_safe(run_ver);
		}
	}
end:
	json_decref(versions_js);
	return run_ver;
}

void thcrap_detour(HMODULE hProc)
{
	size_t mod_name_len = GetModuleFileNameU(hProc, NULL, 0) + 1;
	VLA(char, mod_name, mod_name_len);
	GetModuleFileNameU(hProc, mod_name, mod_name_len);
	log_printf("Applying %s detours to %s...\n", PROJECT_NAME_SHORT(), mod_name);

	iat_detour_apply(hProc);
	VLA_FREE(mod_name);
}

int thcrap_init(const char *run_cfg_fn)
{
	json_t *user_cfg = NULL;
	HMODULE hProc = GetModuleHandle(NULL);

	size_t exe_fn_len = GetModuleFileNameU(NULL, NULL, 0) + 1;
	size_t game_dir_len = GetCurrentDirectory(0, NULL) + 1;
	VLA(char, exe_fn, exe_fn_len);
	VLA(char, game_dir, game_dir_len);

	GetModuleFileNameU(NULL, exe_fn, exe_fn_len);
	GetCurrentDirectory(game_dir_len, game_dir);

	SetCurrentDirectory(dll_dir);

	user_cfg = json_load_file_report(run_cfg_fn);
	runconfig_set(user_cfg);

	{
		json_t *console_val = json_object_get(run_cfg, "console");
		log_init(json_is_true(console_val));
	}

	json_object_set_new(run_cfg, "thcrap_dir", json_string(dll_dir));
	json_object_set_new(run_cfg, "run_cfg_fn", json_string(run_cfg_fn));
	log_printf("Run configuration file: %s\n\n", run_cfg_fn);

	log_printf("Initializing patches...\n");
	{
		json_t *patches = json_object_get(run_cfg, "patches");
		size_t i;
		json_t *patch_info;
		json_array_foreach(patches, i, patch_info) {
			patch_rel_to_abs(patch_info, run_cfg_fn);
			patch_info = patch_init(patch_info);
			json_array_set(patches, i, patch_info);
			json_decref(patch_info);
		}
	}
	stack_show_missing();

	log_printf("EXE file name: %s\n", exe_fn);
	{
		json_t *full_cfg = identify(exe_fn);
		if(full_cfg) {
			json_object_merge(full_cfg, user_cfg);
			runconfig_set(full_cfg);
			json_decref(full_cfg);
		}
	}

	log_printf("---------------------------\n");
	log_printf("Complete run configuration:\n");
	log_printf("---------------------------\n");
	json_dump_log(run_cfg, JSON_INDENT(2));
	log_printf("---------------------------\n");

	log_printf("Game directory: %s\n", game_dir);
	log_printf("Plug-in directory: %s\n", dll_dir);

	log_printf("\nInitializing plug-ins...\n");
	plugin_init(hThcrap);
	plugins_load();

	/**
	  * Potentially dangerous stuff. Do not want!
	  */
	/*
	{
		json_t *patches = json_object_get(run_cfg, "patches");
		size_t i = 1;
		json_t *patch_info;

		json_array_foreach(patches, i, patch_info) {
			const char *archive = json_object_get_string(patch_info, "archive");
			if(archive) {
				SetCurrentDirectory(archive);
				plugins_load();
			}
		}
	}
	*/
	binhacks_apply(json_object_get(run_cfg, "binhacks"));
	breakpoints_apply(json_object_get(run_cfg, "breakpoints"));
	thcrap_detour(hProc);
	SetCurrentDirectory(game_dir);
	VLA_FREE(game_dir);
	VLA_FREE(exe_fn);
	json_decref(user_cfg);
	return 0;
}

int InitDll(HMODULE hDll)
{
	size_t dll_dir_len;

	w32u8_set_fallback_codepage(932);
	InitializeCriticalSection(&cs_file_access);

	exception_init();
	// Needs to be at the lowest level
	win32_detour();
	detour_chain("kernel32.dll", 0,
		"ExitProcess", thcrap_ExitProcess,
		NULL
	);

	hThcrap = hDll;

	// Store the DLL's own directory to load plug-ins later
	dll_dir_len = GetCurrentDirectory(0, NULL) + 1;
	dll_dir = (char*)malloc(dll_dir_len * sizeof(char));
	GetCurrentDirectory(dll_dir_len, dll_dir);
	PathAddBackslashA(dll_dir);

	return 0;
}

void ExitDll(HMODULE hDll)
{
	// Yes, the main thread does not receive a DLL_THREAD_DETACH message
	mod_func_run_all("thread_exit", NULL);
	mod_func_run_all("exit", NULL);
	plugins_close();
	breakpoints_remove();
	run_cfg = json_decref_safe(run_cfg);
	DeleteCriticalSection(&cs_file_access);

	SAFE_FREE(dll_dir);
	detour_exit();
#ifdef _WIN32
#ifdef _DEBUG
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDOUT);
	_CrtDumpMemoryLeaks();
#endif
#endif
	log_exit();
}

VOID WINAPI thcrap_ExitProcess(UINT uExitCode)
{
	ExitDll(NULL);
	// The detour cache is already freed at this point, and this will
	// always be the final detour in the chain, so detour_next() doesn't
	// make any sense here (and would leak memory as well).
	ExitProcess(uExitCode);
}

BOOL APIENTRY DllMain(HMODULE hDll, DWORD ulReasonForCall, LPVOID lpReserved)
{
	switch(ulReasonForCall) {
		case DLL_PROCESS_ATTACH:
			InitDll(hDll);
			break;
		case DLL_PROCESS_DETACH:
			ExitDll(hDll);
			break;
		case DLL_THREAD_DETACH:
			mod_func_run_all("thread_exit", NULL);
			break;
	}
	return TRUE;
}
