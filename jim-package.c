#include <unistd.h>
#include <string.h>

#include <jim.h>
#include <jim-subcmd.h>

/* -----------------------------------------------------------------------------
 * Packages handling
 * ---------------------------------------------------------------------------*/

int Jim_PackageProvide(Jim_Interp *interp, const char *name, const char *ver,
        int flags)
{
    /* If the package was already provided returns an error. */
    if (Jim_FindHashEntry(&interp->packages, name) != NULL) {
        if (flags & JIM_ERRMSG) {
            Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
            Jim_AppendStrings(interp, Jim_GetResult(interp),
                    "package '", name, "' was already provided", NULL);
        }
        return JIM_ERR;
    }
    Jim_AddHashEntry(&interp->packages, name, (char*) ver);
    return JIM_OK;
}

static char *JimFindPackage(Jim_Interp *interp, char **prefixes,
        int prefixc, const char *pkgName)
{
    int i;

    for (i = 0; i < prefixc; i++) {
        char buf[JIM_PATH_LEN];

        if (prefixes[i] == NULL) continue;

        if (strcmp(prefixes[i], ".") == 0) {
            snprintf(buf, sizeof(buf), "%s.tcl", pkgName);
        }
        else {
            snprintf(buf, sizeof(buf), "%s/%s.tcl", prefixes[i], pkgName);
        }

        if (access(buf, R_OK) == 0) {
            return Jim_StrDup(buf);
        }

        snprintf(buf, sizeof(buf), "%s/%s.so", prefixes[i], pkgName);
        if (access(buf, R_OK) == 0) {
            return Jim_StrDup(buf);
        }
    }
    return NULL;
}

/* Search for a suitable package under every dir specified by JIM_LIBPATH,
 * and load it if possible. If a suitable package was loaded with success
 * JIM_OK is returned, otherwise JIM_ERR is returned. */
static int JimLoadPackage(Jim_Interp *interp, const char *name, int flags)
{
    Jim_Obj *libPathObjPtr;
    char **prefixes, *path;
    int prefixc, i, retCode = JIM_ERR;

    libPathObjPtr = Jim_GetGlobalVariableStr(interp, JIM_LIBPATH, JIM_NONE);
    if (libPathObjPtr == NULL) {
        prefixc = 0;
        libPathObjPtr = NULL;
    } else {
        Jim_IncrRefCount(libPathObjPtr);
        Jim_ListLength(interp, libPathObjPtr, &prefixc);
    }

    prefixes = Jim_Alloc(sizeof(char*)*prefixc);
    for (i = 0; i < prefixc; i++) {
            Jim_Obj *prefixObjPtr;
            if (Jim_ListIndex(interp, libPathObjPtr, i,
                    &prefixObjPtr, JIM_NONE) != JIM_OK)
            {
                prefixes[i] = NULL;
                continue;
            }
            prefixes[i] = Jim_StrDup(Jim_GetString(prefixObjPtr, NULL));
    }

    /* Scan every directory for the the first match */
    path = JimFindPackage(interp, prefixes, prefixc, name);
    if (path != NULL) {
        char *p = strrchr(path, '.');
        /* Try to load/source it */
        if (p && strcmp(p, ".tcl") == 0) {
            retCode = Jim_EvalFile(interp, path);
        }
#ifdef with_jim_ext_load
        else {
            retCode = Jim_LoadLibrary(interp, path);
        }
#endif
        Jim_Free(path);
    } else {
        retCode = JIM_ERR;
    }
    for (i = 0; i < prefixc; i++)
        Jim_Free(prefixes[i]);
    Jim_Free(prefixes);
    if (libPathObjPtr)
        Jim_DecrRefCount(interp, libPathObjPtr);
    return retCode;
}

const char *Jim_PackageRequire(Jim_Interp *interp, const char *name, int flags)
{
    Jim_HashEntry *he;

    /* Start with an empty error string */
    Jim_SetResultString(interp, "", 0);

    he = Jim_FindHashEntry(&interp->packages, name);
    if (he == NULL) {
        /* Try to load the package. */
        if (JimLoadPackage(interp, name, flags) == JIM_OK) {
            he = Jim_FindHashEntry(&interp->packages, name);
            if (he == NULL) {
                /* Did not call package provide, so we do it for them */
                Jim_PackageProvide(interp, name, "1.0", 0);

                return "1.0";
            }
            return he->val;
        }

        /* No way... return an error. */
        if (flags & JIM_ERRMSG) {
            int len;
            Jim_Obj *resultObj = Jim_GetResult(interp);
            if (Jim_IsShared(resultObj)) {
                resultObj = Jim_DuplicateObj(interp, resultObj);
            }
            Jim_GetString(resultObj, &len);
            Jim_AppendStrings(interp, resultObj, len ? "\n" : "",
                    "Can't find package '", name, "'", NULL);
            Jim_SetResult(interp, resultObj);
        }
        return NULL;
    } else {
        return he->val;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * package provide name ?version?
 *
 *	This procedure is invoked to declare that a particular version
 *	of a particular package is now present in an interpreter.  There
 *	must not be any other version of this package already
 *	provided in the interpreter.
 *
 * Results:
 *	Returns JIM_OK and sets the package version (or 1.0 if not specified).
 *
 *----------------------------------------------------------------------
 */
static int package_cmd_provide(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *version = "1.0";
    
    if (argc == 2) {
        version = Jim_GetString(argv[1], NULL);
    }
    return Jim_PackageProvide(interp, Jim_GetString(argv[0], NULL), version, JIM_ERRMSG);
}

/*
 *----------------------------------------------------------------------
 *
 * package require name ?version?
 *
 *	This procedure is load a given package.
 *	Note that the version is ignored.
 *
 * Results:
 *	Returns JIM_OK and sets the package version.
 *
 *----------------------------------------------------------------------
 */
static int package_cmd_require(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *ver = Jim_PackageRequire(interp, Jim_GetString(argv[0], NULL), JIM_ERRMSG);

    if (ver == NULL) {
        /* package require failing is important enough to add to the stack */
        return JIM_ERR_ADDSTACK;
    }
    Jim_SetResultString(interp, ver, -1);
    return JIM_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * package list
 *
 *	Returns a list of known packages
 *
 * Results:
 *	Returns JIM_OK and sets a list of known packages.
 *
 *----------------------------------------------------------------------
 */
static int package_cmd_list(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    Jim_HashTableIterator *htiter;
    Jim_HashEntry *he;
    Jim_Obj *listObjPtr = Jim_NewListObj(interp, NULL, 0);
    
    htiter = Jim_GetHashTableIterator(&interp->packages);
    while ((he = Jim_NextHashEntry(htiter)) != NULL) {
        Jim_ListAppendElement(interp, listObjPtr,
                Jim_NewStringObj(interp, he->key, -1));
    }
    Jim_FreeHashTableIterator(htiter);

    Jim_SetResult(interp, listObjPtr);

    return JIM_OK;
}

static const jim_subcmd_type command_table[] = {
    {   .cmd = "provide",
        .args = "name ?version?",
        .function = package_cmd_provide,
        .minargs = 1,
        .maxargs = 2,
        .description = "Indicates that the current script provides the given package"
    },
    {   .cmd = "require",
        .args = "name ?version?",
        .function = package_cmd_require,
        .minargs = 1,
        .maxargs = 2,
        .description = "Loads the given package by looking in standard places"
    },
    {   .cmd = "list",
        .function = package_cmd_list,
        .minargs = 0,
        .maxargs = 0,
        .description = "Lists all known packages"
    },
    { 0 }
};

int Jim_packageInit(Jim_Interp *interp)
{
    Jim_CreateCommand(interp, "package", Jim_SubCmdProc, (void *)command_table, NULL);
    return JIM_OK;
}