/*
 * load.c - load a program
 *
 *   Copyright (c) 2000-2008  Shiro Kawai  <shiro@acm.org>
 * 
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 * 
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the name of the authors nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  $Id: load.c,v 1.123 2008-05-21 10:46:37 shirok Exp $
 */

#define LIBGAUCHE_BODY
#include "gauche.h"
#include "gauche/arch.h"
#include "gauche/port.h"
#include "gauche/builtin-syms.h"

#include <ctype.h>
#include <fcntl.h>

#define LOAD_SUFFIX ".scm"      /* default load suffix */

/*
 * Load file.
 */

typedef struct dlobj_rec dlobj;

/* Static parameters */
static struct {
    /* Load path list */
    ScmGloc *load_path_rec;     /* *load-path*         */
    ScmGloc *dynload_path_rec;  /* *dynamic-load-path* */
    ScmGloc *load_suffixes_rec; /* *load-suffixes*     */
    ScmInternalMutex path_mutex;

    /* Provided features */
    ScmObj provided;            /* List of provided features. */
    ScmObj providing;           /* Alist of features that is being loaded,
                                   and the thread that is loading it. */
    ScmObj waiting;             /* Alist of threads that is waiting for
                                   a feature to being provided, and the
                                   feature that is waited. */
    ScmInternalMutex prov_mutex;
    ScmInternalCond  prov_cv;

    /* Dynamic linking */
    ScmObj dso_suffixes;
    dlobj *dso_list;              /* List of dynamically loaded objects. */
    ScmInternalMutex dso_mutex;
} ldinfo = { (ScmGloc*)&ldinfo, };  /* trick to put ldinfo in .data section */

/* keywords used for load and load-from-port surbs */
static ScmObj key_paths              = SCM_UNBOUND;
static ScmObj key_error_if_not_found = SCM_UNBOUND;
static ScmObj key_environment        = SCM_UNBOUND;
static ScmObj key_macro              = SCM_UNBOUND;
static ScmObj key_ignore_coding      = SCM_UNBOUND;

/* small utility */
static void load_packet_init(ScmLoadPacket *packet)
{
    if (packet) {
        packet->exception = SCM_FALSE;
        packet->loaded = FALSE;
    }
}

/*--------------------------------------------------------------------
 * Scm_LoadFromPort
 * 
 *   The most basic function in the load()-family.  Read an expression
 *   from the given port and evaluates it repeatedly, until it reaches
 *   EOF.  Then the port is closed.   The port is locked by the calling
 *   thread until the operation terminates.
 *
 *   The result of the last evaluation remains on VM.
 *
 *   No matter how the load terminates, either normal or abnormal,
 *   the port is closed, and the current module is restored to the
 *   one when load is called.
 *
 *   FLAGS argument is ignored for now, but reserved for future
 *   extension.  SCM_LOAD_QUIET_NOFILE and SCM_LOAD_IGNORE_CODING
 *   won't have any effect for LoadFromPort; see Scm_Load below.
 *
 *   TODO: if we're using coding-aware port, how should we propagate
 *   locking into the wrapped (original) port?
 */

struct load_packet {
    ScmPort *port;
    ScmModule *prev_module;
    ScmReadContext *ctx;
    ScmObj prev_port;
    ScmObj prev_history;
    ScmObj prev_next;
    int    prev_situation;
};

/* Clean up */
static ScmObj load_after(ScmObj *args, int nargs, void *data)
{
    struct load_packet *p = (struct load_packet *)data;
    ScmVM *vm = Scm_VM();

#ifdef HAVE_GETTIMEOFDAY
    if (SCM_VM_RUNTIME_FLAG_IS_SET(vm, SCM_COLLECT_LOAD_STATS)) {
        struct timeval t0;
        gettimeofday(&t0, NULL);
        vm->stat.loadStat =
            Scm_Cons(Scm_MakeIntegerU(t0.tv_sec*1000000+t0.tv_usec),
                     vm->stat.loadStat);
    }
#endif /*HAVE_GETTIMEOFDAY*/

    Scm_ClosePort(p->port);
    PORT_UNLOCK(p->port);
    Scm_SelectModule(p->prev_module);
    vm->load_port = p->prev_port;
    vm->load_history = p->prev_history;
    vm->load_next = p->prev_next;
    vm->evalSituation = p->prev_situation;
    return SCM_UNDEFINED;
}

/* C-continuation of the loading */
static ScmObj load_cc(ScmObj result, void **data)
{
    struct load_packet *p = (struct load_packet*)(data[0]);
    ScmObj expr = Scm_ReadWithContext(SCM_OBJ(p->port), p->ctx);

    if (!SCM_EOFP(expr)) {
        Scm_VMPushCC(load_cc, data, 1);
        return Scm_VMEval(expr, SCM_FALSE);
    } else {
        return SCM_TRUE;
    }
}

static ScmObj load_body(ScmObj *args, int nargs, void *data)
{
    return load_cc(SCM_NIL, &data);
}

ScmObj Scm_VMLoadFromPort(ScmPort *port, ScmObj next_paths,
                          ScmObj env, int flags)
{
    struct load_packet *p;
    ScmObj port_info;
    ScmVM *vm = Scm_VM();
    ScmModule *module = vm->module;

    /* Sanity check */
    if (!SCM_IPORTP(port))
        Scm_Error("input port required, but got: %S", port);


    if (SCM_PORT_CLOSED_P(port))
        Scm_Error("port already closed: %S", port);
    if (SCM_MODULEP(env)) {
        module = SCM_MODULE(env);
    } else if (!SCM_UNBOUNDP(env) && !SCM_FALSEP(env)) {
        Scm_Error("bad load environment (must be a module or #f): %S", env);
    }

    p = SCM_NEW(struct load_packet);
    p->port = port;
    p->prev_module = vm->module;
    p->prev_port = vm->load_port;
    p->prev_history = vm->load_history;
    p->prev_next = vm->load_next;
    p->prev_situation = vm->evalSituation;

    p->ctx = Scm_MakeReadContext(NULL);
    p->ctx->flags = SCM_READ_LITERAL_IMMUTABLE | SCM_READ_SOURCE_INFO;
    if (SCM_VM_RUNTIME_FLAG_IS_SET(vm, SCM_CASE_FOLD)) {
        p->ctx->flags |= SCM_READ_CASE_FOLD;
    }
    
    vm->load_next = next_paths;
    vm->load_port = SCM_OBJ(port);
    vm->module = module;
    vm->evalSituation = SCM_VM_LOADING;
    if (SCM_PORTP(p->prev_port)) {
        port_info = SCM_LIST2(p->prev_port,
                              Scm_MakeInteger(Scm_PortLine(SCM_PORT(p->prev_port))));
    } else {
        port_info = SCM_LIST1(SCM_FALSE);
    }
    vm->load_history = Scm_Cons(port_info, vm->load_history);

    PORT_LOCK(port, vm);
    return Scm_VMDynamicWindC(NULL, load_body, load_after, p);
}

/* Scheme subr (load-from-port subr &keyword paths environment) */
static ScmObj load_from_port(ScmObj *args, int argc, void *data)
{
    ScmPort *port;
    ScmObj paths, env;
    int flags = 0;
    
    if (!SCM_IPORTP(args[0])) {
        Scm_Error("input port required, but got %S", args[0]);
    }
    port = SCM_PORT(args[0]);
    paths = Scm_GetKeyword(key_paths, args[1], SCM_FALSE);
    env   = Scm_GetKeyword(key_environment, args[1], SCM_FALSE);
    return Scm_VMLoadFromPort(port, paths, env, flags);
}

static SCM_DEFINE_STRING_CONST(load_from_port_NAME, "load-from-port", 14, 14);
static SCM_DEFINE_SUBR(load_from_port_STUB, 1, 1,
                       SCM_OBJ(&load_from_port_NAME), load_from_port,
                       NULL, NULL);

int Scm_LoadFromPort(ScmPort *port, int flags, ScmLoadPacket *packet)
{
    ScmEvalPacket eresult;
    int r;

    load_packet_init(packet);
    if (flags&SCM_LOAD_PROPAGATE_ERROR) {
        Scm_ApplyRec(SCM_OBJ(&load_from_port_STUB), SCM_LIST1(SCM_OBJ(port)));
        if (packet) packet->loaded = TRUE;
        return 0;
    } else {
        r = Scm_Apply(SCM_OBJ(&load_from_port_STUB), SCM_LIST1(SCM_OBJ(port)),
                      &eresult);
        if (packet) {
            packet->exception = eresult.exception;
            packet->loaded = (r >= 0);
        }
        return (r < 0)? -1 : 0;
    }
}

/*---------------------------------------------------------------------
 * Scm_FindFile
 *
 *   Core function to search specified file from the search path *PATH.
 *   Search rules are:
 *   
 *    (1) If given filename begins with "/", "./" or "../", the file is
 *        searched.
 *    (2) If given filename begins with "~", unix-style username
 *        expansion is done, then the resulting file is searched.
 *    (3) Otherwise, the file is searched for each directory in
 *        *load-path*.
 *
 *   If a file is found, it's pathname is returned.  *PATH is modified
 *   to contain the remains of *load-path*, which can be used again to
 *   find next matching filename.
 *   If SUFFIXES is given, filename is assumed not to have suffix,
 *   and suffixes listed in SUFFIXES are tried one by one.
 *   The element in SUFFIXES is directly appended to the FILENAME;
 *   so usually it begins with dot.
 */

static int regfilep(ScmObj path)
{
    struct stat statbuf;
    int r = stat(Scm_GetStringConst(SCM_STRING(path)), &statbuf);
    if (r < 0) return FALSE;
    return S_ISREG(statbuf.st_mode);
}

static ScmObj try_suffixes(ScmObj base, ScmObj suffixes)
{
    ScmObj sp, fpath;
    if (regfilep(base)) return base;
    SCM_FOR_EACH(sp, suffixes) {
        fpath = Scm_StringAppend2(SCM_STRING(base), SCM_STRING(SCM_CAR(sp)));
        if (regfilep(fpath)) return fpath;
    }
    return SCM_FALSE;
}

ScmObj Scm_FindFile(ScmString *filename, ScmObj *paths,
                    ScmObj suffixes, int flags)
{
    u_int size;
    const char *ptr = Scm_GetStringContent(filename, &size, NULL, NULL);
    int use_load_paths = TRUE;
    ScmObj file = SCM_OBJ(filename), fpath = SCM_FALSE;

    if (size == 0) Scm_Error("bad filename to load: \"\"");
    if (*ptr == '~') {
        file = Scm_NormalizePathname(filename, SCM_PATH_EXPAND);
        use_load_paths = FALSE;
    } else if (*ptr == '/'
               || (*ptr == '.' && *(ptr+1) == '/')
               || (*ptr == '.' && *(ptr+1) == '.' && *(ptr+2) == '/')
#if defined(__CYGWIN__) || defined(GAUCHE_WINDOWS)
	       /* support for wicked legacy DOS drive letter */
	       || (isalpha(*ptr) && *(ptr+1) == ':')
#endif /* __CYGWIN__ || GAUCHE_WINDOWS */
	       ) {
        use_load_paths = FALSE;
    }

    if (use_load_paths) {
        ScmObj lpath;
        SCM_FOR_EACH(lpath, *paths) {
            if (!SCM_STRINGP(SCM_CAR(lpath))) {
                Scm_Warn("*load-path* contains invalid element: %S", *paths);
            }
            fpath = Scm_StringAppendC(SCM_STRING(SCM_CAR(lpath)), "/", 1, 1);
            fpath = Scm_StringAppend2(SCM_STRING(fpath), SCM_STRING(file));
            fpath = try_suffixes(fpath, suffixes);
            if (!SCM_FALSEP(fpath)) break;
        }
        if (SCM_PAIRP(lpath)) {
            *paths = SCM_CDR(lpath);
            return SCM_OBJ(fpath);
        } else if (!(flags&SCM_LOAD_QUIET_NOFILE)) {
            Scm_Error("cannot find file %S in *load-path* %S", file, *paths);
        } else {
            *paths = SCM_NIL;
        }
    } else {
        *paths = SCM_NIL;
        fpath = try_suffixes(file, suffixes);
        if (!SCM_FALSEP(fpath)) return fpath;
        if (!(flags&SCM_LOAD_QUIET_NOFILE)) {
            Scm_Error("cannot find file %S to load", file);
        }
    }
    return SCM_FALSE;
}

/*---------------------------------------------------------------------
 * Scm_Load
 *
 *  Scheme's load().
 * 
 *  filename   - name of the file.  can be sans suffix.
 *  load_paths - list of pathnames or #f.   If #f, system's load path
 *               is used.
 *  env        - a module where the forms are evaluated, or #f.
 *               If #f, the current module is used.
 *  flags      - combination of bit flags
 *               SCM_LOAD_QUIET_NOFILE, SCM_LOAD_IGNORE_CODING
 */

ScmObj Scm_VMLoad(ScmString *filename, ScmObj load_paths,
                  ScmObj env, int flags)
{
    ScmObj port, truename, suffixes;
    ScmVM *vm = Scm_VM();
    int errorp = !(flags&SCM_LOAD_QUIET_NOFILE);
    int ignore_coding = flags&SCM_LOAD_IGNORE_CODING;

    suffixes = SCM_GLOC_GET(ldinfo.load_suffixes_rec);
    if (!SCM_PAIRP(load_paths)) load_paths = Scm_GetLoadPath();
    truename = Scm_FindFile(filename, &load_paths, suffixes, flags);
    if (SCM_FALSEP(truename)) return SCM_FALSE;

#ifdef HAVE_GETTIMEOFDAY
    if (SCM_VM_RUNTIME_FLAG_IS_SET(vm, SCM_COLLECT_LOAD_STATS)) {
        struct timeval t0;
        gettimeofday(&t0, NULL);
        vm->stat.loadStat =
            Scm_Acons(truename,
                      Scm_MakeIntegerU(t0.tv_sec*1000000+t0.tv_usec),
                      vm->stat.loadStat);
    }
#endif /*HAVE_GETTIMEOFDAY*/
    if (SCM_VM_RUNTIME_FLAG_IS_SET(vm, SCM_LOAD_VERBOSE)) {
        int len = Scm_Length(vm->load_history);
        SCM_PUTZ(";;", 2, SCM_CURERR);
        while (len-- > 0) SCM_PUTC(' ', SCM_CURERR);
        Scm_Printf(SCM_CURERR, "Loading %A...\n", truename);
    }

    port = Scm_OpenFilePort(Scm_GetStringConst(SCM_STRING(truename)),
                            O_RDONLY, SCM_PORT_BUFFER_FULL, 0);
    if (SCM_FALSEP(port)) {
        if (errorp) Scm_Error("file %S exists, but couldn't open.", truename);
        else        return SCM_FALSE;
    }
    if (!ignore_coding) {
        port = Scm_MakeCodingAwarePort(SCM_PORT(port));
    }
    return Scm_VMLoadFromPort(SCM_PORT(port), load_paths, env, flags);
}

/* Scheme subr (%load filename &keyword paths error-if-not-found
                                        environment aware-coding) */
static ScmObj load(ScmObj *args, int argc, void *data)
{
    ScmString *file;
    ScmObj paths, env;
    int flags = 0;

    if (!SCM_STRINGP(args[0])) {
        Scm_Error("string required, but got %S", args[0]);
    }
    file = SCM_STRING(args[0]);
    paths = Scm_GetKeyword(key_paths, args[1], SCM_FALSE);
    env   = Scm_GetKeyword(key_environment, args[1], SCM_FALSE);
    if (SCM_FALSEP(Scm_GetKeyword(key_error_if_not_found, args[1], SCM_TRUE)))
        flags |= SCM_LOAD_QUIET_NOFILE;
    if (!SCM_FALSEP(Scm_GetKeyword(key_ignore_coding, args[1], SCM_FALSE)))
        flags |= SCM_LOAD_IGNORE_CODING;
    return Scm_VMLoad(file, paths, env, flags);
}

static SCM_DEFINE_STRING_CONST(load_NAME, "load", 4, 4);
static SCM_DEFINE_SUBR(load_STUB, 1, 1, SCM_OBJ(&load_NAME), load, NULL, NULL);


int Scm_Load(const char *cpath, int flags, ScmLoadPacket *packet)
{
    ScmObj f = SCM_MAKE_STR_COPYING(cpath);
    ScmObj options = SCM_NIL;
    ScmEvalPacket eresult;
    
    if (flags&SCM_LOAD_QUIET_NOFILE) {
        options = Scm_Cons(key_error_if_not_found,
                           Scm_Cons(SCM_FALSE, options));
    }
    if (flags&SCM_LOAD_IGNORE_CODING) {
        options = Scm_Cons(key_ignore_coding,
                           Scm_Cons(SCM_TRUE, options));
    }

    load_packet_init(packet);
    if (flags&SCM_LOAD_PROPAGATE_ERROR) {
        ScmObj r = Scm_ApplyRec(SCM_OBJ(&load_STUB), Scm_Cons(f, options));
        if (packet) {
            packet->loaded = !SCM_FALSEP(r);
        }
        return 0;
    } else {
        int r = Scm_Apply(SCM_OBJ(&load_STUB), Scm_Cons(f, options), &eresult);
        if (packet) {
            packet->exception = eresult.exception;
            packet->loaded = (r > 0 && !SCM_FALSEP(eresult.results[0]));
        }
        return (r >= 0)? 0 : -1;
    }
}

/*
 * Utilities
 */

ScmObj Scm_GetLoadPath(void)
{
    ScmObj paths;
    (void)SCM_INTERNAL_MUTEX_LOCK(ldinfo.path_mutex);
    paths = Scm_CopyList(ldinfo.load_path_rec->value);
    (void)SCM_INTERNAL_MUTEX_UNLOCK(ldinfo.path_mutex);
    return paths;
}

ScmObj Scm_GetDynLoadPath(void)
{
    ScmObj paths;
    (void)SCM_INTERNAL_MUTEX_LOCK(ldinfo.path_mutex);
    paths = Scm_CopyList(ldinfo.dynload_path_rec->value);
    (void)SCM_INTERNAL_MUTEX_UNLOCK(ldinfo.path_mutex);
    return paths;
}

static ScmObj break_env_paths(const char *envname)
{
    const char *e = getenv(envname);
#ifndef GAUCHE_WINDOWS
    char delim = ':';
#else  /*GAUCHE_WINDOWS*/
    char delim = ';';
#endif /*GAUCHE_WINDOWS*/

    if (e == NULL) {
	return SCM_NIL;
    } else if (Scm_IsSugid()) {
        /* don't trust env when setugid'd */
        return SCM_NIL;
    } else {
	return Scm_StringSplitByChar(SCM_STRING(SCM_MAKE_STR_COPYING(e)),
				     delim);
    }
}

/* Add CPATH to the current list of load path.  The path is
 * added before the current list, unless AFTERP is true.
 * The existence of CPATH is not checked.
 *
 * Besides load paths, existence of directories CPATH/$ARCH and
 * CPATH/../$ARCH is checked, where $ARCH is the system architecture
 * signature, and if found, it is added to the dynload_path.  If
 * no such directory is found, CPATH itself is added to the dynload_path.
 */
ScmObj Scm_AddLoadPath(const char *cpath, int afterp)
{
    ScmObj spath = SCM_MAKE_STR_COPYING(cpath);
    ScmObj dpath;
    ScmObj r;
    struct stat statbuf;

    /* check dynload path */
    dpath = Scm_StringAppendC(SCM_STRING(spath), "/", 1, 1);
    dpath = Scm_StringAppendC(SCM_STRING(dpath), Scm_HostArchitecture(),-1,-1);
    if (stat(Scm_GetStringConst(SCM_STRING(dpath)), &statbuf) < 0
        || !S_ISDIR(statbuf.st_mode)) {
        dpath = Scm_StringAppendC(SCM_STRING(spath), "/../", 4, 4);
        dpath = Scm_StringAppendC(SCM_STRING(dpath), Scm_HostArchitecture(),-1,-1);
        if (stat(Scm_GetStringConst(SCM_STRING(dpath)), &statbuf) < 0
            || !S_ISDIR(statbuf.st_mode)) {
            dpath = spath;
        }
    }

    (void)SCM_INTERNAL_MUTEX_LOCK(ldinfo.path_mutex);
    if (!SCM_PAIRP(ldinfo.load_path_rec->value)) {
        ldinfo.load_path_rec->value = SCM_LIST1(spath);
    } else if (afterp) {
        ldinfo.load_path_rec->value =
            Scm_Append2(ldinfo.load_path_rec->value, SCM_LIST1(spath));
    } else {
        ldinfo.load_path_rec->value = Scm_Cons(spath, ldinfo.load_path_rec->value);
    }
    r = ldinfo.load_path_rec->value;

    if (!SCM_PAIRP(ldinfo.dynload_path_rec->value)) {
        ldinfo.dynload_path_rec->value = SCM_LIST1(dpath);
    } else if (afterp) {
        ldinfo.dynload_path_rec->value =
            Scm_Append2(ldinfo.dynload_path_rec->value, SCM_LIST1(dpath));
    } else {
        ldinfo.dynload_path_rec->value =
            Scm_Cons(dpath, ldinfo.dynload_path_rec->value);
    }
    (void)SCM_INTERNAL_MUTEX_UNLOCK(ldinfo.path_mutex);
    
    return r;
}

/*------------------------------------------------------------------
 * Dynamic linking
 */

/* The API to load object file dynamically differ among platforms.
 * We include the platform-dependent implementations (dl_*.c) that
 * provides a common API:
 *
 *   void *dl_open(const char *pathname)
 *     Dynamically loads the object file specified by PATHNAME,
 *     and returns its handle.   On failure, returns NULL.
 *
 *     PATHNAME is guaranteed to contain directory names, so this function
 *     doesn't need to look it up in the search paths.
 *     The caller also checks whether pathname is already loaded or not,
 *     so this function doesn't need to worry about duplicate loads.
 *     This function should have the semantics equivalent to the
 *     RTLD_NOW|RTLD_GLOBAL of dlopen().
 *
 *     We don't call with NULL as PATHNAME; dlopen() returns the handle
 *     of the calling program itself in such a case, but we never need that
 *     behavior.
 *
 *   ScmDynloadInitFn dl_sym(void *handle, const char *symbol)
 *     Finds the address of SYMBOL in the dl_openModule()-ed module
 *     HANDLE.
 *
 *   void dl_close(void *handle)
 *     Closes the opened module.  This can only be called when we couldn't
 *     find the initialization function in the module; once the initialization
 *     function is called, we don't have a safe way to remove the module.
 *
 *   const char *dl_error(void)
 *     Returns the last error occurred on HANDLE in the dl_* function.
 *
 * Notes:
 *   - The caller takes care of mutex so that dl_ won't be called from
 *     more than one thread at a time, and no other thread calls
 *     dl_* functions between dl_open and dl_error (so that dl_open
 *     can store the error info in global variable).
 *
 * Since this API assumes the caller does a lot of work, the implementation
 * should be much simpler than implementing fully dlopen()-compatible
 * functions.
 */

typedef void (*ScmDynLoadInitFn)(void);

enum dlobj_state {
    DLOBJ_NONE,             /* dlopen and dlsym haven't completed. */
    DLOBJ_LOADED,           /* dlopened and initfn found, but not init'ed */
    DLOBJ_INITIALIZED       /* initialized and ready to use */
};

struct dlobj_rec {
    dlobj *next;                /* chain */
    const char *path;           /* pathname for DSO, including suffix */
    enum dlobj_state state;     /* state of loading */
    void *handle;               /* whatever dl_open returned */
    ScmVM *loader;              /* VM who's loading this.  NULL once done. */
    ScmDynLoadInitFn initfn;    /* initialization fn */
    ScmInternalMutex mutex;
    ScmInternalCond  cv;
};


/* NB: we rely on dlcompat library for dlopen instead of using dl_darwin.c
   for now; Boehm GC requires dlopen when compiled with pthread, so there's
   not much point to avoid dlopen here. */
#if defined(HAVE_DLOPEN)
#include "dl_dlopen.c"
#elif defined(GAUCHE_WINDOWS)
#include "dl_win.c"
#else
#include "dl_dummy.c"
#endif

/* Derives initialization function name from the module file name.
   This function _always_ appends underscore before the symbol.
   The dynamic loader first tries the symbol without underscore,
   then tries with underscore. */
#define DYNLOAD_PREFIX   "_Scm_Init_"

static const char *get_dynload_initfn(const char *filename)
{
    const char *head, *tail, *s;
    char *name, *d;

    head = strrchr(filename, '/');
    if (head == NULL) head = filename;
    else head++;
    tail = strchr(head, '.');
    if (tail == NULL) tail = filename + strlen(filename);

    name = SCM_NEW_ATOMIC2(char *, sizeof(DYNLOAD_PREFIX) + tail - head);
    strcpy(name, DYNLOAD_PREFIX);
    for (s = head, d = name + sizeof(DYNLOAD_PREFIX) - 1; s < tail; s++, d++) {
        if (isalnum(*s)) *d = tolower(*s);
        else *d = '_';
    }
    *d = '\0';
    return name;
}

static dlobj *make_dlobj(const char *path)
{
    dlobj *z = SCM_NEW(dlobj);
    z->next = NULL;
    z->path = path;
    z->loader = Scm_VM();
    z->state = DLOBJ_NONE;
    z->initfn = NULL;
    (void)SCM_INTERNAL_MUTEX_INIT(z->mutex);
    (void)SCM_INTERNAL_COND_INIT(z->cv);
    return z;
}

static dlobj *find_or_add_dlobj(dlobj *newobj)
{
    dlobj *z = NULL;
    (void)SCM_INTERNAL_MUTEX_LOCK(ldinfo.dso_mutex);
    for (z = ldinfo.dso_list; z; z = z->next) {
        if (strcmp(z->path, newobj->path) == 0) break;
    }
    if (z == NULL) {
        newobj->next = ldinfo.dso_list;
        ldinfo.dso_list = z = newobj;
    }
    (void)SCM_INTERNAL_MUTEX_UNLOCK(ldinfo.dso_mutex);
    return z;
}

#if 0  /* Turned off libtool *.la support.  Not sure if we need this. */
/* Aux fn to get a parameter value from *.la file line */
static const char *get_la_val(const char *start)
{
    const char *end;
    if (start[0] == '\'') start++;
    end = strrchr(start, '\'');
    if (end) {
        char *p = SCM_NEW_ATOMIC2(char*, (end-start+1));
        memcpy(p, start, (end-start));
        p[end-start] = '\0';
        return (const char*)p;
    } else {
        return start;
    }
}

/* We found libtool *.la file.  Retrieve DSO path from it.
   This routine make some assumption on .la file.  I couldn't
   find a formal specification of .la file format. */
static ScmObj find_so_from_la(ScmString *lafile)
{
    ScmObj f = Scm_OpenFilePort(Scm_GetStringConst(lafile),
                                O_RDONLY, SCM_PORT_BUFFER_FULL, 0);
    const char *dlname = NULL, *libdir = NULL;
    int installed = FALSE;
    
    for (;;) {
        const char *cline;
        ScmObj line = Scm_ReadLineUnsafe(SCM_PORT(f));
        if (SCM_EOFP(line)) break;
        cline = Scm_GetStringConst(SCM_STRING(line));
        if (strncmp(cline, "dlname=", sizeof("dlname=")-1) == 0) {
            dlname = get_la_val(cline+sizeof("dlname=")-1);
            continue;
        }
        if (strncmp(cline, "libdir=", sizeof("libdir=")-1) == 0) {
            libdir = get_la_val(cline+sizeof("libdir=")-1);
            continue;
        }
        if (strncmp(cline, "installed=yes", sizeof("installed=yes")-1) == 0) {
            installed = TRUE;
            continue;
        }
    }
    Scm_ClosePort(SCM_PORT(f));
    if (!dlname) return SCM_FALSE;
    if (installed && libdir) {
        ScmObj path = Scm_StringAppendC(SCM_STRING(SCM_MAKE_STR(libdir)),
                                        "/", 1, 1);
        path = Scm_StringAppend2(SCM_STRING(path),
                                 SCM_STRING(SCM_MAKE_STR(dlname)));
        /*Scm_Printf(SCM_CURERR, "Z=%S\n", path);*/
        if (regfilep(path)) return path;
    } else {
        ScmObj dir = Scm_DirName(lafile);
        ScmObj path = Scm_StringAppendC(SCM_STRING(dir),
                                        "/" SCM_LIBTOOL_OBJDIR "/",
                                        sizeof("/" SCM_LIBTOOL_OBJDIR "/")-1,
                                        sizeof("/" SCM_LIBTOOL_OBJDIR "/")-1);
        path = Scm_StringAppend2(SCM_STRING(path),
                                 SCM_STRING(SCM_MAKE_STR(dlname)));
        /*Scm_Printf(SCM_CURERR, "T=%S\n", path);*/
        if (regfilep(path)) return path;
    }
    return SCM_FALSE;
}
#endif

/* Dynamically load the specified object by FILENAME.
   FILENAME must not contain the system's suffix (.so, for example).
*/
ScmObj Scm_DynLoad(ScmString *filename, ScmObj initfn, int export_)
{
    ScmObj reqname, truename, load_paths = Scm_GetDynLoadPath();
    const char *cpath, *initname;
    dlobj *newdlo, *dlo;

    truename = Scm_FindFile(filename, &load_paths, ldinfo.dso_suffixes, TRUE);
    if (SCM_FALSEP(truename)) {
        Scm_Error("can't find dlopen-able module %S", filename);
    }
    reqname = truename;         /* save requested name */
    cpath = Scm_GetStringConst(SCM_STRING(truename));

#if 0  /* Turned off libtool *.la support.  Not sure if we need this. */
    if ((suff = strrchr(cpath, '.')) && strcmp(suff, ".la") == 0) {
        truename = find_so_from_la(SCM_STRING(truename));
        if (SCM_FALSEP(truename)) {
            Scm_Error("couldn't find dlopen-able module from libtool archive file %s", cpath);
        }
        cpath = Scm_GetStringConst(SCM_STRING(truename));
    }
#endif

    if (SCM_STRINGP(initfn)) {
        ScmObj _initfn = Scm_StringAppend2(SCM_STRING(Scm_MakeString("_", 1, 1, 0)),
                                           SCM_STRING(initfn));
        initname = Scm_GetStringConst(SCM_STRING(_initfn));
    } else {
        /* NB: we use requested name to derive initfn name, instead of
           the one given in libtool .la file.  For example, on cygwin,
           the actual DLL that libtool library libfoo.la points to is
           named cygfoo.dll; we still want Scm_Init_libfoo in that case,
           not Scm_Init_cygfoo. */
        initname = get_dynload_initfn(Scm_GetStringConst(SCM_STRING(reqname)));
    }

    newdlo = make_dlobj(cpath);
    dlo = find_or_add_dlobj(newdlo);

    if (dlo != newdlo) {
        /* somebody has tried to load this DSO. */
        (void)SCM_INTERNAL_MUTEX_LOCK(dlo->mutex);
        while (dlo->state != DLOBJ_INITIALIZED) {
            if (dlo->loader == NULL) {
                /* loading is abandoned.  we take it over. */
                dlo->loader = newdlo->loader;  /* this is our VM */
                break;
            } else {
                (void)SCM_INTERNAL_COND_WAIT(dlo->cv, dlo->mutex);
            }
        }
        (void)SCM_INTERNAL_MUTEX_UNLOCK(dlo->mutex);

        if (dlo->state == DLOBJ_INITIALIZED) {
            /* it is already loaded. we just return. */
            return SCM_TRUE;
        }
        /* FALLTHROUGH */
    }

    /* At this moment, dlo is owned by our thread (either we're the
       first one to load, or we took over the abandoned dlo).  Note that
       only the loader can touch dlo->state; so it is safe to inspect
       it here without locking it. */
    SCM_UNWIND_PROTECT {
        ScmVM *vm = Scm_VM();

        switch (dlo->state) {
        case DLOBJ_NONE:
            /* First, we dl_open the DSO. */
            if (SCM_VM_RUNTIME_FLAG_IS_SET(vm, SCM_LOAD_VERBOSE)) {
                int len = Scm_Length(vm->load_history);
                SCM_PUTZ(";;", 2, SCM_CURERR);
                while (len-- > 0) SCM_PUTC(' ', SCM_CURERR);
                Scm_Printf(SCM_CURERR, "Dynamically Loading %s...\n", cpath);
            }
            dlo->handle = dl_open(cpath);
            if (dlo->handle == NULL) {
                const char *err = dl_error();
                if (err == NULL) {
                    Scm_Error("failed to link %S dynamically", filename);
                } else {
                    Scm_Error("failed to link %S dynamically: %s", filename, err);
                }
                /*NOTREACHED*/
            }
            /* locate initfn.  initname always has '_'.  Whether the actual
               symbol dl_sym returns has '_' or not depends on the platform,
               so we first try without '_', then '_'. */
            dlo->initfn = dl_sym(dlo->handle, initname+1);
            if (dlo->initfn == NULL) {
                dlo->initfn = (void(*)(void))dl_sym(dlo->handle, initname);
                if (dlo->initfn == NULL) {
                    dl_close(dlo->handle);
                    dlo->handle = NULL;
                    Scm_Error("dynamic linking of %S failed: couldn't find initialization function %s", filename, initname);
                    /*NOTREACHED*/
                }
            }
            dlo->state = DLOBJ_LOADED;
            /*FALLTHROUGH*/
        case DLOBJ_LOADED:
            SCM_ASSERT(dlo->initfn != NULL);
            /* Call initialization function.  note that there can be arbitrary
               complex stuff done within func(), including evaluation of
               Scheme procedures and/or calling dynamic-load for other
               object.  There's a chance that, with some contrived example,
               func() can trigger the dynamic loading of the same file we're
               loading right now.  However, if the code follows the Gauche's
               standard module structure, such circular dependency is detected
               by Scm_Load, so we don't worry about it here. */
            dlo->initfn();
            /* Alright.  Everything seems fine. */
            dlo->state = DLOBJ_INITIALIZED;
            /*FALLTHROUGH*/
        default:
            /*Nothing to do*/
            break;
        }
    } SCM_WHEN_ERROR {
        (void)SCM_INTERNAL_MUTEX_LOCK(dlo->mutex);
        dlo->loader = NULL;
        (void)SCM_INTERNAL_COND_BROADCAST(dlo->cv);
        (void)SCM_INTERNAL_MUTEX_UNLOCK(dlo->mutex);
        SCM_NEXT_HANDLER;
    } SCM_END_PROTECT;

    /* All is done. */
    (void)SCM_INTERNAL_MUTEX_LOCK(dlo->mutex);
    dlo->loader = NULL;
    (void)SCM_INTERNAL_COND_BROADCAST(dlo->cv);
    (void)SCM_INTERNAL_MUTEX_UNLOCK(dlo->mutex);

    return SCM_TRUE;
}

/*------------------------------------------------------------------
 * Require and provide
 */

/* STk's require takes a string.  SLIB's require takes a symbol.
   For now, I allow only a string. */
/* Note that require and provide is recognized at compile time. */

/* [Preventing Race Condition]
 *
 *   Besides the list of provided features (ldinfo.provided), the
 *   system keeps two kind of global assoc list for transient information.
 *
 *   ldinfo.providing keeps a list of (<feature> . <thread>), where
 *   <thread> is currently loading a file for <feature>.
 *   ldinfo.waiting keeps a list of (<thread> . <feature>), where
 *   <thread> is waiting for <feature> to be provided.
 *
 *   Scm_Require first checks ldinfo.provided list; if the feature is
 *   already provided, no problem; just return.
 *   If not, ldinfo.providing is searched.  If the feature is being provided
 *   by some other thread, the calling thread pushes itself onto
 *   ldinfo.waiting list and waits for the feature to be provided.
 *
 *   There may be a case that the feature dependency forms a loop because
 *   of bug.  An error should be signaled in such a case, rather than going
 *   to deadlock.   So, when the calling thread finds the required feature
 *   is in the ldinfo.providing alist, it checks the waiting chain of
 *   features, and no threads are waiting for a feature being provided by
 *   the calling thread.
 */

int Scm_Require(ScmObj feature, int flags, ScmLoadPacket *packet)
{
    ScmObj filename;
    ScmVM *vm = Scm_VM();
    ScmObj provided, providing, p, q;
    int loop = FALSE, r;
    ScmLoadPacket xresult;

    load_packet_init(packet);
    if (!SCM_STRINGP(feature)) {
        ScmObj e = Scm_MakeError(Scm_Sprintf("require: string expected, but got %S\n", feature));
        if (flags&SCM_LOAD_PROPAGATE_ERROR) Scm_Raise(e);
        else {
            if (packet) packet->exception = e;
            return -1;
        }
    }

    (void)SCM_INTERNAL_MUTEX_LOCK(ldinfo.prov_mutex);
    do {
        provided = Scm_Member(feature, ldinfo.provided, SCM_CMP_EQUAL);
        if (!SCM_FALSEP(provided)) break;
        providing = Scm_Assoc(feature, ldinfo.providing, SCM_CMP_EQUAL);
        if (SCM_FALSEP(providing)) break;

        /* Checks for dependency loop */
        p = providing;
        SCM_ASSERT(SCM_PAIRP(p));
        if (SCM_CDR(p) == SCM_OBJ(vm)) {
            loop = TRUE;
            break;
        }
        
        for (;;) {
            q = Scm_Assq(SCM_CDR(p), ldinfo.waiting);
            if (SCM_FALSEP(q)) break;
            SCM_ASSERT(SCM_PAIRP(q));
            p = Scm_Assoc(SCM_CDR(q), ldinfo.providing, SCM_CMP_EQUAL);
            SCM_ASSERT(SCM_PAIRP(p));
            if (SCM_CDR(p) == SCM_OBJ(vm)) {
                loop = TRUE;
                break;
            }
        }
        if (loop) break;
        ldinfo.waiting = Scm_Acons(SCM_OBJ(vm), feature, ldinfo.waiting);
        (void)SCM_INTERNAL_COND_WAIT(ldinfo.prov_cv, ldinfo.prov_mutex);
        ldinfo.waiting = Scm_AssocDeleteX(SCM_OBJ(vm), ldinfo.waiting, SCM_CMP_EQ);
        continue;
    } while (0);
    if (!loop && SCM_FALSEP(provided)) {
        ldinfo.providing = Scm_Acons(feature, SCM_OBJ(vm), ldinfo.providing);
    }
    (void)SCM_INTERNAL_MUTEX_UNLOCK(ldinfo.prov_mutex);

    if (loop) {
        ScmObj e = Scm_MakeError(Scm_Sprintf("a loop is detected in the require dependency involving feature %S", feature));
        if (flags&SCM_LOAD_PROPAGATE_ERROR) Scm_Raise(e);
        else {
            if (packet) packet->exception = e;
            return -1;
        }
    }
        
    if (!SCM_FALSEP(provided)) return 0;
    filename = Scm_StringAppendC(SCM_STRING(feature), ".scm", 4, 4);
    r = Scm_Load(Scm_GetStringConst(SCM_STRING(filename)), 0, &xresult);
    if (packet) packet->exception = xresult.exception;

    if (r < 0) {
        (void)SCM_INTERNAL_MUTEX_LOCK(ldinfo.prov_mutex);
        ldinfo.providing = Scm_AssocDeleteX(feature, ldinfo.providing, SCM_CMP_EQUAL);
        (void)SCM_INTERNAL_COND_SIGNAL(ldinfo.prov_cv);
        (void)SCM_INTERNAL_MUTEX_UNLOCK(ldinfo.prov_mutex);
        if (flags&SCM_LOAD_PROPAGATE_ERROR) Scm_Raise(xresult.exception);
        else return -1;
    }

    (void)SCM_INTERNAL_MUTEX_LOCK(ldinfo.prov_mutex);
    ldinfo.providing = Scm_AssocDeleteX(feature, ldinfo.providing, SCM_CMP_EQUAL);
    (void)SCM_INTERNAL_COND_SIGNAL(ldinfo.prov_cv);
    (void)SCM_INTERNAL_MUTEX_UNLOCK(ldinfo.prov_mutex);
    if (packet) packet->loaded = TRUE;
    return 0;
}

ScmObj Scm_Provide(ScmObj feature)
{
    if (!SCM_STRINGP(feature))
        Scm_Error("provide: string expected, but got %S\n", feature);
    (void)SCM_INTERNAL_MUTEX_LOCK(ldinfo.prov_mutex);
    if (SCM_FALSEP(Scm_Member(feature, ldinfo.provided, SCM_CMP_EQUAL))) {
        ldinfo.provided = Scm_Cons(feature, ldinfo.provided);
    }
    if (!SCM_FALSEP(Scm_Member(feature, ldinfo.providing, SCM_CMP_EQUAL))) {
        ldinfo.providing = Scm_DeleteX(feature, ldinfo.providing, SCM_CMP_EQUAL);
    }
    (void)SCM_INTERNAL_COND_SIGNAL(ldinfo.prov_cv);
    (void)SCM_INTERNAL_MUTEX_UNLOCK(ldinfo.prov_mutex);
    return feature;
}

int Scm_ProvidedP(ScmObj feature)
{
    int r;
    (void)SCM_INTERNAL_MUTEX_LOCK(ldinfo.prov_mutex);
    r = !SCM_FALSEP(Scm_Member(feature, ldinfo.provided, SCM_CMP_EQUAL));
    (void)SCM_INTERNAL_MUTEX_UNLOCK(ldinfo.prov_mutex);
    return r;
}

/*------------------------------------------------------------------
 * Autoload
 */

static void autoload_print(ScmObj obj, ScmPort *out, ScmWriteContext *ctx)
{
    Scm_Printf(out, "#<autoload %A::%A (%A)>",
               SCM_AUTOLOAD(obj)->module->name,
               SCM_AUTOLOAD(obj)->name, SCM_AUTOLOAD(obj)->path);
}

SCM_DEFINE_BUILTIN_CLASS_SIMPLE(Scm_AutoloadClass, autoload_print);

ScmObj Scm_MakeAutoload(ScmModule *where,
                        ScmSymbol *name,
                        ScmString *path,
                        ScmSymbol *import_from)
{
    ScmAutoload *adata = SCM_NEW(ScmAutoload);
    SCM_SET_CLASS(adata, SCM_CLASS_AUTOLOAD);
    adata->name = name;
    adata->module = where;
    adata->path = path;
    adata->import_from = import_from;
    adata->loaded = FALSE;
    adata->value = SCM_UNBOUND;
    (void)SCM_INTERNAL_MUTEX_INIT(adata->mutex);
    (void)SCM_INTERNAL_COND_INIT(adata->cv);
    adata->locker = NULL;
    return SCM_OBJ(adata);
}

void Scm_DefineAutoload(ScmModule *where,
                        ScmObj file_or_module,
                        ScmObj list)
{
    ScmString *path = NULL;
    ScmSymbol *import_from = NULL;
    ScmObj ep;

    if (SCM_STRINGP(file_or_module)) {
        path = SCM_STRING(file_or_module);
    } else if (SCM_SYMBOLP(file_or_module)) {
        import_from = SCM_SYMBOL(file_or_module);
        path = SCM_STRING(Scm_ModuleNameToPath(import_from));
    } else {
        Scm_Error("autoload: string or symbol required, but got %S",
                  file_or_module);
    }
    SCM_FOR_EACH(ep, list) {
        ScmObj entry = SCM_CAR(ep);
        if (SCM_SYMBOLP(entry)) {
            Scm_Define(where, SCM_SYMBOL(entry),
                       Scm_MakeAutoload(where, SCM_SYMBOL(entry),
                                        path, import_from));
        } else if (SCM_PAIRP(entry)
                   && SCM_EQ(key_macro, SCM_CAR(entry))
                   && SCM_PAIRP(SCM_CDR(entry))
                   && SCM_SYMBOLP(SCM_CADR(entry))) {
            ScmSymbol *sym = SCM_SYMBOL(SCM_CADR(entry));
            ScmObj autoload = Scm_MakeAutoload(where, sym, path, import_from);
            Scm_Define(where, sym,
                       Scm_MakeMacroAutoload(sym, SCM_AUTOLOAD(autoload)));
        } else {
            Scm_Error("autoload: bad autoload symbol entry: %S", entry);
        }
    }
}


ScmObj Scm_ResolveAutoload(ScmAutoload *adata, int flags)
{
    int circular = FALSE;
    ScmModule *prev_module;
    ScmVM *vm = Scm_VM();
    
    /* shortcut in case if somebody else already did the job. */
    if (adata->loaded) return adata->value;

    /* check to see if this autoload is recursive.  if so, we just return
       SCM_UNBOUND and let the caller handle the issue (NB: it isn't
       necessarily an error.  For example, define-method searches if
       a generic function of the same name is already defined; if the
       name is set autoload and define-method is in the file that's being
       autoloaded, define-method finds the name is an autoload that points
       the currently autoloaded file.)
       we have to be careful to exclude the case that when one thread is
       resolving autoload another thread enters here and sees this autoload
       is already being resolved.
     */
    if ((adata->locker == NULL || adata->locker == vm)
        && !SCM_FALSEP(Scm_Assoc(SCM_OBJ(adata->path),
                                 ldinfo.providing,
                                 SCM_CMP_EQUAL))) {
        return SCM_UNBOUND;
    }

    /* obtain the lock to load this autoload */
    (void)SCM_INTERNAL_MUTEX_LOCK(adata->mutex);
    do {
        if (adata->loaded) break;
        if (adata->locker == NULL) {
            adata->locker = vm;
        } else if (adata->locker == vm) {
            /* bad circular dependency */
            circular = TRUE;
        } else if (adata->locker->state == SCM_VM_TERMINATED) {
            /* the loading thread have died prematurely.
               let's take over the task. */
            adata->locker = vm;
        } else {
            (void)SCM_INTERNAL_COND_WAIT(adata->cv, adata->mutex);
            continue;
        }
    } while (0);
    SCM_INTERNAL_MUTEX_UNLOCK(adata->mutex);
    if (adata->loaded) {
        /* ok, somebody did the work for me.  just use the result. */
        return adata->value;
    }
    
    if (circular) {
        /* Since we have already checked recursive loading, it isn't normal
           if we reach here.  Right now I have no idea how this happens, but
           just in case we raise an error. */
        adata->locker = NULL;
        SCM_INTERNAL_COND_BROADCAST(adata->cv);
        Scm_Error("Attempted to trigger the same autoload %S#%S recursively.  Maybe circular autoload dependency?\n",
                  adata->module, adata->name);
    }

    prev_module = vm->module;
    SCM_UNWIND_PROTECT {
        vm->module = adata->module;
        Scm_Require(SCM_OBJ(adata->path), SCM_LOAD_PROPAGATE_ERROR, NULL);
        vm->module = prev_module;
    
        if (adata->import_from) {
            /* autoloaded file defines import_from module.  we need to
               import the binding individually. */
            ScmModule *m = Scm_FindModule(adata->import_from,
                                          SCM_FIND_MODULE_QUIET);
            ScmGloc *f, *g;
            if (m == NULL) {
                Scm_Error("Trying to autoload module %S from file %S, but the file doesn't define such a module",
                          adata->import_from, adata->path);
            }
            f = Scm_FindBinding(SCM_MODULE(m), adata->name, 0);
            g = Scm_FindBinding(adata->module, adata->name, 0);
            if (f == NULL) {
                Scm_Error("ZIMBER: %S %S\n", m, adata->name);
            }
            SCM_ASSERT(f != NULL);
            SCM_ASSERT(g != NULL);
            adata->value = SCM_GLOC_GET(f);
            if (SCM_UNBOUNDP(adata->value) || SCM_AUTOLOADP(adata->value)) {
                Scm_Error("Autoloaded symbol %S is not defined in the module %S",
                          adata->name, adata->import_from);
            }
            SCM_GLOC_SET(g, adata->value);
        } else {
            /* Normal import.  The binding must have been inserted to
               adata->module */
            ScmGloc *g = Scm_FindBinding(adata->module, adata->name, 0);
            SCM_ASSERT(g != NULL);
            adata->value = SCM_GLOC_GET(g);
            if (SCM_UNBOUNDP(adata->value) || SCM_AUTOLOADP(adata->value)) {
                Scm_Error("Autoloaded symbol %S is not defined in the file %S",
                          adata->name, adata->path);
            }
        }
    } SCM_WHEN_ERROR {
        adata->locker = NULL;
        vm->module = prev_module;
        SCM_INTERNAL_COND_BROADCAST(adata->cv);
        SCM_NEXT_HANDLER;
    } SCM_END_PROTECT;

    adata->loaded = TRUE;
    adata->locker = NULL;
    SCM_INTERNAL_COND_BROADCAST(adata->cv);
    return adata->value;
}

/*------------------------------------------------------------------
 * Compatibility stuff
 */

void Scm__LoadFromPortCompat(ScmPort *port, int flags)
{
    Scm_LoadFromPort(port, flags|SCM_LOAD_PROPAGATE_ERROR, NULL);
}

int  Scm__LoadCompat(const char *file, int flags)
{
    return (0 == Scm_Load(file, flags|SCM_LOAD_PROPAGATE_ERROR, NULL));
}

ScmObj Scm__RequireCompat(ScmObj feature)
{
    Scm_Require(feature, SCM_LOAD_PROPAGATE_ERROR, NULL);
    return SCM_TRUE;
}


/*------------------------------------------------------------------
 * Initialization
 */

void Scm__InitLoad(void)
{
    ScmModule *m = Scm_SchemeModule();
    ScmObj init_load_path, init_dynload_path, init_load_suffixes, t;

    init_load_path = t = SCM_NIL;
    SCM_APPEND(init_load_path, t, break_env_paths("GAUCHE_LOAD_PATH"));
    SCM_APPEND1(init_load_path, t, Scm_SiteLibraryDirectory());
    SCM_APPEND1(init_load_path, t, Scm_LibraryDirectory());

    init_dynload_path = t = SCM_NIL;
    SCM_APPEND(init_dynload_path, t, break_env_paths("GAUCHE_DYNLOAD_PATH"));
    SCM_APPEND1(init_dynload_path, t, Scm_SiteArchitectureDirectory());
    SCM_APPEND1(init_dynload_path, t, Scm_ArchitectureDirectory());

    init_load_suffixes = t = SCM_NIL;
    SCM_APPEND1(init_load_suffixes, t, SCM_MAKE_STR(LOAD_SUFFIX));

    (void)SCM_INTERNAL_MUTEX_INIT(ldinfo.path_mutex);
    (void)SCM_INTERNAL_MUTEX_INIT(ldinfo.prov_mutex);
    (void)SCM_INTERNAL_COND_INIT(ldinfo.prov_cv);
    (void)SCM_INTERNAL_MUTEX_INIT(ldinfo.dso_mutex);

    key_paths = SCM_MAKE_KEYWORD("paths");
    key_error_if_not_found = SCM_MAKE_KEYWORD("error-if-not-found");
    key_environment = SCM_MAKE_KEYWORD("environment");
    key_macro = SCM_MAKE_KEYWORD("macro");
    key_ignore_coding = SCM_MAKE_KEYWORD("ignore-coding");
    
    SCM_DEFINE(m, "load-from-port", SCM_OBJ(&load_from_port_STUB));
    SCM_DEFINE(m, "load", SCM_OBJ(&load_STUB));

#define DEF(rec, sym, val) \
    rec = SCM_GLOC(Scm_Define(m, SCM_SYMBOL(sym), val))

    DEF(ldinfo.load_path_rec,    SCM_SYM_LOAD_PATH, init_load_path);
    DEF(ldinfo.dynload_path_rec, SCM_SYM_DYNAMIC_LOAD_PATH, init_dynload_path);
    DEF(ldinfo.load_suffixes_rec, SCM_SYM_LOAD_SUFFIXES, init_load_suffixes);

    ldinfo.provided =
        SCM_LIST5(SCM_MAKE_STR("srfi-2"), /* and-let* */
                  SCM_MAKE_STR("srfi-6"), /* string ports (builtin) */
                  SCM_MAKE_STR("srfi-8"), /* receive (builtin) */
                  SCM_MAKE_STR("srfi-10"), /* #, (builtin) */
                  SCM_MAKE_STR("srfi-17")  /* set! (builtin) */
            );
    ldinfo.providing = SCM_NIL;
    ldinfo.waiting = SCM_NIL;
    ldinfo.dso_suffixes = SCM_LIST2(SCM_MAKE_STR(".la"),
                                    SCM_MAKE_STR("." SHLIB_SO_SUFFIX));
    ldinfo.dso_list = NULL;
}
