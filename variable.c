/* Yash: yet another shell */
/* variable.c: deals with shell variables and parameters */
/* (C) 2007-2010 magicant */

/* This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#include "common.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#if HAVE_GETTEXT
# include <libintl.h>
#endif
#include "builtin.h"
#include "configm.h"
#include "exec.h"
#include "expand.h"
#include "hashtable.h"
#include "input.h"
#include "option.h"
#include "parser.h"
#include "path.h"
#include "plist.h"
#include "sig.h"
#include "strbuf.h"
#include "util.h"
#include "variable.h"
#include "xfnmatch.h"
#include "yash.h"
#if YASH_ENABLE_LINEEDIT
# include "lineedit/complete.h"
# include "lineedit/lineedit.h"
# include "lineedit/terminfo.h"
#endif


static const wchar_t *const path_variables[PA_count] = {
    [PA_PATH]     = L VAR_PATH,
    [PA_CDPATH]   = L VAR_CDPATH,
    [PA_LOADPATH] = L VAR_YASH_LOADPATH,
};


/* variable environment (= set of variables) */
/* not to be confused with environment variables */
typedef struct environ_T {
    struct environ_T *parent;      /* parent environment */
    struct hashtable_T contents;   /* hashtable containing variables */
    bool is_temporary;             /* for temporary assignment? */
    char **paths[PA_count];
} environ_T;
/* `contents' is a hashtable from (wchar_t *) to (variable_T *).
 * A variable name may contain any characters except L'\0' and L'=', though
 * assignment syntax allows only limited characters.
 * Variable names starting with L'=' are used for special purposes.
 * The positional parameter is treated as an array whose name is L"=".
 * Note that the number of positional parameters is offset by 1 against the
 * array index. */
/* An environment whose `is_temporary' is true is used for temporary variables. 
 * Temporary variables may be exported but cannot be readonly.
 * The elements of `paths' are arrays of the pathnames contained in the
 * $PATH, $CDPATH and $YASH_LOADPATH variables. They are NULL if the
 * corresponding variables are not set. */
#define VAR_positional "="

/* flags for variable attributes */
typedef enum vartype_T {
    VF_SCALAR,
    VF_ARRAY,
    VF_EXPORT   = 1 << 2,
    VF_READONLY = 1 << 3,
    VF_NODELETE = 1 << 4,
} vartype_T;
#define VF_MASK ((1 << 2) - 1)
/* For all variables, the variable type is either VF_SCALAR or VF_ARRAY,
 * possibly OR'ed with other flags. */

/* type of variables */
typedef struct variable_T {
    vartype_T v_type;
    union {
	wchar_t *value;
	struct {
	    void **vals;
	    size_t valc;
	} array;
    } v_contents;
    void (*v_getter)(struct variable_T *var);
} variable_T;
#define v_value v_contents.value
#define v_vals  v_contents.array.vals
#define v_valc  v_contents.array.valc
/* `v_vals' is a NULL-terminated array of pointers to wide strings.
 * `v_valc' is, of course, the number of elements in `v_vals'.
 * `v_value', `v_vals' and the elements of `v_vals' are `free'able.
 * `v_value' is NULL if the variable is declared but not yet assigned.
 * `v_vals' is always non-NULL, but it may contain no elements.
 * `v_getter' is the setter function, which is reset to NULL on reassignment.*/

/* type of shell functions (defined later) */
typedef struct function_T function_T;


static void varvaluefree(variable_T *v)
    __attribute__((nonnull));
static void varfree(variable_T *v);
static void varkvfree(kvpair_T kv);
static void varkvfree_reexport(kvpair_T kv);

static void init_pwd(void);

static variable_T *search_variable(const wchar_t *name)
    __attribute__((pure,nonnull));
static variable_T *search_array_and_check_if_changeable(const wchar_t *name)
    __attribute__((pure,nonnull));
static void update_environment(const wchar_t *name)
    __attribute__((nonnull));
static void reset_locale(const wchar_t *name)
    __attribute__((nonnull));
static void reset_locale_category(const wchar_t *name, int category)
    __attribute__((nonnull));
static variable_T *new_global(const wchar_t *name)
    __attribute__((nonnull));
static variable_T *new_local(const wchar_t *name)
    __attribute__((nonnull));
static variable_T *new_temporary(const wchar_t *name)
    __attribute__((nonnull));
static variable_T *new_variable(const wchar_t *name, scope_T scope)
    __attribute__((nonnull));
static void xtrace_variable(const wchar_t *name, const wchar_t *value)
    __attribute__((nonnull));
static void xtrace_array(const wchar_t *name, void *const *values)
    __attribute__((nonnull));
static void get_all_variables_rec(
	hashtable_T *table, environ_T *env, bool global)
    __attribute__((nonnull));

static void lineno_getter(variable_T *var)
    __attribute__((nonnull));
static void random_getter(variable_T *var)
    __attribute__((nonnull));
static unsigned next_random(void);

static void variable_set(const wchar_t *name, variable_T *var)
    __attribute__((nonnull(1)));

static char **convert_path_array(void **ary)
    __attribute__((malloc,warn_unused_result));
static void add_to_list_no_dup(plist_T *list, char *s)
    __attribute__((nonnull(1)));
static void reset_path(path_T name, variable_T *var);

static void funcfree(function_T *f);
static void funckvfree(kvpair_T kv);

static void hash_all_commands_recursively(const command_T *c);
static void hash_all_commands_in_and_or(const and_or_T *ao);
static void hash_all_commands_in_if(const ifcommand_T *ic);
static void hash_all_commands_in_case(const caseitem_T *ci);
static void tryhash_word_as_command(const wordunit_T *w);


/* the current environment */
static environ_T *current_env;
/* the top-level environment (the farthest from the current) */
static environ_T *first_env;

/* whether $RANDOM is functioning as a random number */
static bool random_active;

/* hashtable from function names (wchar_t *) to functions (function_T *). */
static hashtable_T functions;


/* Frees the value of the specified variable (but not the variable itself). */
/* This function does not change the value of `*v'. */
void varvaluefree(variable_T *v)
{
    switch (v->v_type & VF_MASK) {
	case VF_SCALAR:
	    free(v->v_value);
	    break;
	case VF_ARRAY:
	    recfree(v->v_vals, free);
	    break;
    }
}

/* Frees the specified variable. */
void varfree(variable_T *v)
{
    if (v) {
	varvaluefree(v);
	free(v);
    }
}

/* Frees the specified key-value pair of a variable name and a variable. */
void varkvfree(kvpair_T kv)
{
    free(kv.key);
    varfree(kv.value);
}

/* Calls `variable_set' and `update_environment' for `kv.key' and
 * calls `varkvfree'. */
void varkvfree_reexport(kvpair_T kv)
{
    variable_set(kv.key, NULL);
    if (((variable_T *) kv.value)->v_type & VF_EXPORT)
	update_environment(kv.key);
    varkvfree(kv);
}

/* Initializes the top-level environment. */
void init_variables(void)
{
    assert(first_env == NULL && current_env == NULL);
    first_env = current_env = xmalloc(sizeof *current_env);
    current_env->parent = NULL;
    current_env->is_temporary = false;
    ht_init(&current_env->contents, hashwcs, htwcscmp);
//    for (size_t i = 0; i < PA_count; i++)
//	current_env->paths[i] = NULL;

    ht_init(&functions, hashwcs, htwcscmp);

    /* add all the existing environment variables to the variable environment */
    for (char **e = environ; *e; e++) {
	wchar_t *we = malloc_mbstowcs(*e);
	if (!we)
	    continue;

	wchar_t *eqp = wcschr(we, L'=');
	variable_T *v = xmalloc(sizeof *v);
	v->v_type = VF_SCALAR | VF_EXPORT;
	v->v_value = eqp ? xwcsdup(eqp + 1) : NULL;
	v->v_getter = NULL;
	if (eqp) {
	    *eqp = L'\0';
	    we = xrealloc(we, sizeof *we * (eqp - we + 1));
	}
	varkvfree(ht_set(&current_env->contents, we, v));
    }

    /* initialize path according to $PATH etc. */
    for (size_t i = 0; i < PA_count; i++)
	current_env->paths[i] = decompose_paths(getvar(path_variables[i]));

    /* set $IFS */
    set_variable(L VAR_IFS, xwcsdup(DEFAULT_IFS), SCOPE_GLOBAL, false);

    /* set $LINENO */
    {
	variable_T *v = new_variable(L VAR_LINENO, false);
	assert(v != NULL);
	v->v_type = VF_SCALAR | (v->v_type & VF_EXPORT);
	v->v_value = NULL;
	v->v_getter = lineno_getter;
	// variable_set(VAR_LINENO, v);
	if (v->v_type & VF_EXPORT)
	    update_environment(L VAR_LINENO);
    }

    /* set $MAILCHECK */
    if (!getvar(L VAR_MAILCHECK))
	set_variable(L VAR_MAILCHECK, xwcsdup(L"600"), SCOPE_GLOBAL, false);

    /* set $PS1~4 */
    {
	const wchar_t *ps1 =
	    !posixly_correct ? L"\\$ " : (geteuid() != 0) ? L"$ " : L"# ";
	set_variable(L VAR_PS1, xwcsdup(ps1),   SCOPE_GLOBAL, false);
	set_variable(L VAR_PS2, xwcsdup(L"> "), SCOPE_GLOBAL, false);
	set_variable(L VAR_PS4, xwcsdup(L"+ "), SCOPE_GLOBAL, false);
    }

    /* set $PWD */
    init_pwd();

    /* export $OLDPWD */
    {
	variable_T *v = new_global(L VAR_OLDPWD);
	assert(v != NULL);
	v->v_type |= VF_EXPORT;
	variable_set(L VAR_OLDPWD, v);
    }

    /* set $PPID */
    set_variable(L VAR_PPID, malloc_wprintf(L"%jd", (intmax_t) getppid()),
	    SCOPE_GLOBAL, false);

    /* set $OPTIND */
    set_variable(L VAR_OPTIND, xwcsdup(L"1"), SCOPE_GLOBAL, false);

    /* set $RANDOM */
    if (!posixly_correct && !getvar(L VAR_RANDOM)) {
	variable_T *v = new_variable(L VAR_RANDOM, false);
	assert(v != NULL);
	v->v_type = VF_SCALAR;
	v->v_value = NULL;
	v->v_getter = random_getter;
	random_active = true;
	srand(shell_pid);
    } else {
	random_active = false;
    }

    /* set $YASH_LOADPATH */
    set_variable(L VAR_YASH_LOADPATH, xwcsdup(L YASH_DATADIR),
	    SCOPE_GLOBAL, false);

    /* set $YASH_VERSION */
    set_variable(L VAR_YASH_VERSION, xwcsdup(L PACKAGE_VERSION),
	    SCOPE_GLOBAL, false);
}

/* Reset the value of $PWD if
 *  - $PWD doesn't exist, or
 *  - the value of $PWD isn't an absolute path, or
 *  - the value of $PWD isn't the actual current directory, or
 *  - the value of $PWD isn't canonicalized. */
void init_pwd(void)
{
    const char *pwd = getenv(VAR_PWD);
    if (!pwd || pwd[0] != '/' || !is_same_file(pwd, "."))
	goto set;
    const wchar_t *wpwd = getvar(L VAR_PWD);
    if (!wpwd || !is_normalized_path(wpwd))
	goto set;
    return;

    char *newpwd;
    wchar_t *wnewpwd;
set:
    newpwd = xgetcwd();
    if (!newpwd) {
	xerror(errno, Ngt("failed to set $PWD"));
	return;
    }
    wnewpwd = realloc_mbstowcs(newpwd);
    if (!wnewpwd) {
	xerror(0, Ngt("failed to set $PWD"));
	return;
    }
    set_variable(L VAR_PWD, wnewpwd, SCOPE_GLOBAL, true);
}

/* Searches for a variable with the specified name.
 * Returns NULL if none was found. */
variable_T *search_variable(const wchar_t *name)
{
    for (environ_T *env = current_env; env; env = env->parent) {
	variable_T *var = ht_get(&env->contents, name).value;
	if (var)
	    return var;
    }
    return NULL;
}

/* Searches for an array with the specified name and checks if it is not read-
 * only. If unsuccessful, prints an error message and returns NULL. */
variable_T *search_array_and_check_if_changeable(const wchar_t *name)
{
    variable_T *array = search_variable(name);
    if (!array || (array->v_type & VF_MASK) != VF_ARRAY) {
	xerror(0, Ngt("no such array $%ls"), name);
	return NULL;
    } else if (array->v_type & VF_READONLY) {
	xerror(0, Ngt("$%ls is read-only"), name);
	return NULL;
    }
    return array;
}

/* Update the value in `environ' for the variable with the specified name.
 * `name' must not contain '='. */
void update_environment(const wchar_t *name)
{
    char *mname = malloc_wcstombs(name);
    if (!mname)
	return;
    for (environ_T *env = current_env; env; env = env->parent) {
	variable_T *var = ht_get(&env->contents, name).value;
	if (var && (var->v_type & VF_EXPORT)) {
	    char *value;
	    switch (var->v_type & VF_MASK) {
		case VF_SCALAR:
		    if (!var->v_value)
			continue;
		    value = malloc_wcstombs(var->v_value);
		    break;
		case VF_ARRAY:
		    value = realloc_wcstombs(joinwcsarray(var->v_vals, L":"));
		    break;
		default:
		    assert(false);
	    }
	    if (value) {
		if (setenv(mname, value, true) < 0)
		    xerror(errno, Ngt("failed to set environment variable $%s"),
			    mname);
		free(value);
	    } else {
		xerror(EILSEQ, Ngt("failed to set environment variable $%s"),
			mname);
	    }
	    goto done;
	}
    }
    if (unsetenv(mname) < 0)
	xerror(errno, Ngt("failed to unset environment variable $%s"), mname);
done:
    free(mname);
}

/* Resets the locate settings for the specified variable.
 * If `name' is not any of "LANG", "LC_ALL", etc., does nothing. */
void reset_locale(const wchar_t *name)
{
    if (wcscmp(name, L VAR_LANG) == 0) {
	goto reset_locale_all;
    } else if (wcsncmp(name, L"LC_", 3) == 0) {
	/* POSIX forbids resetting LC_CTYPE even if the value of the variable
	 * is changed, but we do reset LC_CTYPE if the shell is interactive and
	 * not in the POSIXly-correct mode. */
	if (wcscmp(name + 3, L VAR_LC_ALL + 3) == 0) {
reset_locale_all:
	    reset_locale_category(L VAR_LC_COLLATE, LC_COLLATE);
	    if (!posixly_correct && is_interactive_now)
		reset_locale_category(L VAR_LC_CTYPE, LC_CTYPE);
	    reset_locale_category(L VAR_LC_MESSAGES, LC_MESSAGES);
	    reset_locale_category(L VAR_LC_MONETARY, LC_MONETARY);
	    reset_locale_category(L VAR_LC_NUMERIC, LC_NUMERIC);
	    reset_locale_category(L VAR_LC_TIME, LC_TIME);
	} else if (wcscmp(name + 3, L VAR_LC_COLLATE + 3) == 0) {
	    reset_locale_category(L VAR_LC_COLLATE, LC_COLLATE);
	} else if (wcscmp(name + 3, L VAR_LC_CTYPE + 3) == 0) {
	    if (!posixly_correct && is_interactive_now)
		reset_locale_category(L VAR_LC_CTYPE, LC_CTYPE);
	} else if (wcscmp(name + 3, L VAR_LC_MESSAGES + 3) == 0) {
	    reset_locale_category(L VAR_LC_MESSAGES, LC_MESSAGES);
	} else if (wcscmp(name + 3, L VAR_LC_MONETARY + 3) == 0) {
	    reset_locale_category(L VAR_LC_MONETARY, LC_MONETARY);
	} else if (wcscmp(name + 3, L VAR_LC_NUMERIC + 3) == 0) {
	    reset_locale_category(L VAR_LC_NUMERIC, LC_NUMERIC);
	} else if (wcscmp(name + 3, L VAR_LC_TIME + 3) == 0) {
	    reset_locale_category(L VAR_LC_TIME, LC_TIME);
	}
    }
}

/* Resets the locale of the specified category.
 * `name' must be one of the `LC_*' constants except LC_ALL. */
void reset_locale_category(const wchar_t *name, int category)
{
    const wchar_t *v = getvar(L VAR_LC_ALL);
    if (!v) {
	v = getvar(name);
	if (!v) {
	    v = getvar(L VAR_LANG);
	    if (!v)
		v = L"";
	}
    }
    char *sv = malloc_wcstombs(v);
    if (sv) {
	setlocale(category, sv);
	free(sv);
    }
}

/* Creates a new scalar variable that has no value.
 * If the variable already exists, it is returned without change. So the return
 * value may be an array variable or it may be a scalar variable with a value.
 * Temporary variables with the `name' are cleared if any. */
variable_T *new_global(const wchar_t *name)
{
    variable_T *var;
    for (environ_T *env = current_env; env; env = env->parent) {
	var = ht_get(&env->contents, name).value;
	if (var) {
	    if (env->is_temporary) {
		varkvfree_reexport(ht_remove(&env->contents, name));
		continue;
	    }
	    return var;
	}
    }
    var = xmalloc(sizeof *var);
    var->v_type = VF_SCALAR;
    var->v_value = NULL;
    var->v_getter = NULL;
    ht_set(&first_env->contents, xwcsdup(name), var);
    return var;
}

/* Creates a new scalar variable that has no value.
 * If the variable already exists, it is returned without change. So the return
 * value may be an array variable or it may be a scalar variable with a value.
 * Temporary variables with the `name' are cleared if any. */
variable_T *new_local(const wchar_t *name)
{
    environ_T *env = current_env;
    while (env->is_temporary) {
	varkvfree_reexport(ht_remove(&env->contents, name));
	env = env->parent;
    }
    variable_T *var = ht_get(&env->contents, name).value;
    if (var)
	return var;
    var = xmalloc(sizeof *var);
    var->v_type = VF_SCALAR;
    var->v_value = NULL;
    var->v_getter = NULL;
    ht_set(&env->contents, xwcsdup(name), var);
    return var;
}

/* Creates a new scalar variable that has no value.
 * If the variable already exists, it is returned without change. So the return
 * value may be an array variable or it may be a scalar variable with a value.
 * The current environment must be a temporary environment.
 * If there is a readonly non-temporary variable with the specified name, it is
 * returned (no new temporary variable is created). */
variable_T *new_temporary(const wchar_t *name)
{
    environ_T *env = current_env;
    assert(env->is_temporary);

    /* check if readonly */
    variable_T *var = search_variable(name);
    if (var && (var->v_type & VF_READONLY))
	return var;

    var = ht_get(&env->contents, name).value;
    if (var)
	return var;
    var = xmalloc(sizeof *var);
    var->v_type = VF_SCALAR;
    var->v_value = NULL;
    var->v_getter = NULL;
    ht_set(&env->contents, xwcsdup(name), var);
    return var;
}

/* Creates a new variable with the specified name if there is none.
 * If the variable already exists, it is cleared and returned.
 *
 * On error, an error message is printed to the standard error and NULL is
 * returned. Otherwise, a (new) variable is returned.
 * `v_type' is the only valid member of the variable and the all members
 * (including `v_type') must be initialized by the caller. If `v_type' of the
 * return value has the VF_EXPORT flag set, the caller must call
 * `update_environment'. */
variable_T *new_variable(const wchar_t *name, scope_T scope)
{
    variable_T *var;

    switch (scope) {
	case SCOPE_GLOBAL:  var = new_global(name);     break;
	case SCOPE_LOCAL:   var = new_local(name);      break;
	case SCOPE_TEMP:    var = new_temporary(name);  break;
	default:            assert(false);
    }
    if (var->v_type & VF_READONLY) {
	xerror(0, Ngt("$%ls is read-only"), name);
	return NULL;
    } else {
	varvaluefree(var);
	return var;
    }
}

/* Creates a scalar variable with the specified name and value.
 * `value' must be a `free'able string or NULL. The caller must not modify or
 * free `value' hereafter, whether or not this function is successful.
 * If `export' is true, the variable is exported (i.e., the VF_EXPORT flag is
 * set to the variable). But this function does not reset an existing VF_EXPORT
 * flag if `export' is false.
 * Returns true iff successful. On error, an error message is printed to the
 * standard error. */
bool set_variable(
	const wchar_t *name, wchar_t *value, scope_T scope, bool export)
{
    variable_T *var = new_variable(name, scope);
    if (!var) {
	free(value);
	return false;
    }

    var->v_type = VF_SCALAR
	| (var->v_type & (VF_EXPORT | VF_NODELETE))
	| (export ? VF_EXPORT : 0);
    var->v_value = value;
    var->v_getter = NULL;

    variable_set(name, var);
    if (var->v_type & VF_EXPORT)
	update_environment(name);
    return true;
}

/* Creates an array variable with the specified name and values.
 * `values' is a NULL-terminated array of pointers to wide strings. It is used
 * as the contents of the array variable hereafter, so you must not modify or
 * free the array or its elements whether or not this function succeeds.
 * `values' and its elements must be `free'able.
 * `count' is the number of elements in `values'. If `count' is zero, the
 * number is counted in this function.
 * Returns the set array iff successful. On error, an error message is printed
 * to the standard error and NULL is returned. */
variable_T *set_array(
	const wchar_t *name, size_t count, void **values, scope_T scope)
{
    variable_T *var = new_variable(name, scope);
    if (!var) {
	recfree(values, free);
	return NULL;
    }

    var->v_type = VF_ARRAY | (var->v_type & (VF_EXPORT | VF_NODELETE));
    var->v_vals = values;
    var->v_valc = count ? count : plcount(var->v_vals);
    var->v_getter = NULL;

    variable_set(name, var);
    if (var->v_type & VF_EXPORT)
	update_environment(name);
    return var;
}

/* Changes the value of the specified array element.
 * `name' must be the name of an existing array.
 * `index' is the index of the element (counted from zero).
 * `value' is the new value, which must be a `free'able string. Since `value' is
 * used as the contents of the array element, you must not modify or free
 * `value' after this function returned (whether successful or not).
 * Returns true iff successful. An error message is printed on failure. */
bool set_array_element(const wchar_t *name, size_t index, wchar_t *value)
{
    variable_T *array = search_array_and_check_if_changeable(name);
    if (!array)
	goto fail;
    if (array->v_valc <= index)
	goto invalid_index;

    free(array->v_vals[index]);
    array->v_vals[index] = value;
    if (array->v_type & VF_EXPORT)
	update_environment(name);
    return true;

invalid_index:
    xerror(0,
	    Ngt("index %zu is out of range "
		"(the actual size of array $%ls is %zu)"),
	    index + 1, name, array->v_valc);
fail:
    free(value);
    return false;
}

/* Sets the positional parameters of the current environment.
 * The existent parameters are cleared.
 * `values' is an NULL-terminated array of pointers to wide strings.
 * `values[0]' will be the new $1, `values[1]' $2, and so on.
 * When a new non-temporary environment is created, this function must be called
 * at least once before the environment is used by the user. */
void set_positional_parameters(void *const *values)
{
    set_array(L VAR_positional, 0, duparray(values, copyaswcs), SCOPE_LOCAL);
}

/* Performs the specified assignments.
 * If `shopt_xtrace' is true, traces are printed to the standard error.
 * If `temp' is true, the variables are assigned in the current environment,
 * which must be a temporary environment. Otherwise, they are assigned globally.
 * If `export' is true, the variables are exported (i.e., the VF_EXPORT flag is
 * set to the variables). But this function does not reset an existing VF_EXPORT
 * flag if `export' is false.
 * Returns true iff successful. On error, already-assigned variables are not
 * restored to the previous values. */
bool do_assignments(const assign_T *assign, bool temp, bool export)
{
    if (temp)
	assert(current_env->is_temporary);

    scope_T scope = temp ? SCOPE_TEMP : SCOPE_GLOBAL;
    while (assign) {
	wchar_t *value;
	int count;
	void **values;

	switch (assign->a_type) {
	    case A_SCALAR:
		value = expand_single(assign->a_scalar, tt_multi);
		if (!value)
		    return false;
		value = unescapefree(value);
		if (shopt_xtrace)
		    xtrace_variable(assign->a_name, value);
		if (!set_variable(assign->a_name, value, scope, export))
		    return false;
		break;
	    case A_ARRAY:
		if (!expand_line(assign->a_array, &count, &values))
		    return false;
		assert(values != NULL);
		if (shopt_xtrace)
		    xtrace_array(assign->a_name, values);
		if (!set_array(assign->a_name, count, values, scope))
		    return false;
		break;
	}
	assign = assign->next;
    }
    return true;
}

/* Pushes a trace of the specified variable assignment to the xtrace buffer. */
void xtrace_variable(const wchar_t *name, const wchar_t *value)
{
    xwcsbuf_T *buf = get_xtrace_buffer();
    wb_wprintf(buf, L" %ls=%ls", name, value);
}

/* Pushes a trace of the specified array assignment to the xtrace buffer. */
void xtrace_array(const wchar_t *name, void *const *values)
{
    xwcsbuf_T *buf = get_xtrace_buffer();

    wb_wprintf(buf, L" %ls=(", name);
    if (*values) {
	for (;;) {
	    wb_cat(buf, *values);
	    values++;
	    if (!*values)
		break;
	    wb_wccat(buf, L' ');
	}
    }
    wb_wccat(buf, L')');
}

/* Gets the value of the specified scalar variable.
 * Cannot be used for special parameters such as $$ and $@.
 * Returns the value of the variable, or NULL if not found.
 * The return value must not be modified or `free'ed by the caller and
 * is valid until the variable is re-assigned or unset. */
const wchar_t *getvar(const wchar_t *name)
{
    variable_T *var = search_variable(name);
    if (var && (var->v_type & VF_MASK) == VF_SCALAR) {
	if (var->v_getter) {
	    var->v_getter(var);
	    if ((var->v_type & VF_MASK) != VF_SCALAR)
		return NULL;
	}
	return var->v_value;
    }
    return NULL;
}

/* Returns the value(s) of the specified variable/array as an array.
 * The return value's type is `struct get_variable'. It has three members:
 * `type', `count' and `values'.
 * `type' is the type of the result:
 *    GV_NOTFOUND:     no such variable/array
 *    GV_SCALAR:       a normal scalar variable
 *    GV_ARRAY:        an array of zero or more values
 *    GV_ARRAY_CONCAT: an array whose values should be concatenated by caller
 * `values' is the array containing the value(s) of the variable/array.
 * A scalar value (GV_SCALAR) is returned as a newly-malloced array containing
 * exactly one newly-malloced wide string. An array (GV_ARRAY*) is returned as
 * a NULL-terminated array of pointers to wide strings, which must not be
 * changed or freed.  If no such variable is found (GV_NOTFOUND), `values' is
 * NULL.
 * `count' is the number of elements in `values'. */
struct get_variable get_variable(const wchar_t *name)
{
    struct get_variable result = {
	.type = GV_NOTFOUND, .count = 0, .values = NULL, };
    wchar_t *value;
    variable_T *var;

    if (name[0] == L'\0')
	return result;
    if (name[1] == L'\0') {
	/* `name' is one-character long: check if it's a special parameter */
	switch (name[0]) {
	    case L'*':
		result.type = GV_ARRAY_CONCAT;
		goto positional_parameters;
	    case L'@':
		result.type = GV_ARRAY;
positional_parameters:
		var = search_variable(L VAR_positional);
		assert(var != NULL && (var->v_type & VF_MASK) == VF_ARRAY);
		result.count = var->v_valc;
		result.values = var->v_vals;
		return result;
	    case L'#':
		var = search_variable(L VAR_positional);
		assert(var != NULL && (var->v_type & VF_MASK) == VF_ARRAY);
		value = malloc_wprintf(L"%zu", var->v_valc);
		goto return_single;
	    case L'?':
		value = malloc_wprintf(L"%d", laststatus);
		goto return_single;
	    case L'-':
		value = get_hyphen_parameter();
		goto return_single;
	    case L'$':
		value = malloc_wprintf(L"%jd", (intmax_t) shell_pid);
		goto return_single;
	    case L'!':
		value = malloc_wprintf(L"%jd", (intmax_t) lastasyncpid);
		goto return_single;
	    case L'0':
		value = xwcsdup(command_name);
		goto return_single;
	}
    }

    if (iswdigit(name[0])) {
	/* `name' starts with a digit: a positional parameter */
	wchar_t *nameend;
	errno = 0;
	uintmax_t v = wcstoumax(name, &nameend, 10);
	if (errno || *nameend != L'\0')
	    return result;  /* not a number or overflow */
	var = search_variable(L VAR_positional);
	assert(var != NULL && (var->v_type & VF_MASK) == VF_ARRAY);
	if (v == 0 || var->v_valc < v)
	    return result;  /* index out of bounds */
	value = xwcsdup(var->v_vals[v - 1]);
	goto return_single;
    }

    /* now it should be a normal variable */
    var = search_variable(name);
    if (var) {
	if (var->v_getter)
	    var->v_getter(var);
	switch (var->v_type & VF_MASK) {
	    case VF_SCALAR:
		value = var->v_value ? xwcsdup(var->v_value) : NULL;
		goto return_single;
	    case VF_ARRAY:
		result.type = GV_ARRAY;
		result.count = var->v_valc;
		result.values = var->v_vals;
		return result;
	}
    }
    return result;

return_single:  /* return a scalar as a one-element array */
    if (!value)
	return result;
    result.type = GV_SCALAR;
    result.count = 1;
    result.values = xmalloc(2 * sizeof *result.values);
    result.values[0] = value;
    result.values[1] = NULL;
    return result;
}

/* Gathers all variables in the specified environment and adds them to the
 * specified hashtable.
 * If `global' is true, variables in the all ancestor environments are also
 * included (except the ones hidden by local variables).
 * Keys and values added to the hashtable must not be modified or freed. */
void get_all_variables_rec(hashtable_T *table, environ_T *env, bool global)
{
    if (env->parent && (global || env->is_temporary))
	get_all_variables_rec(table, env->parent, global);

    size_t i = 0;
    kvpair_T kv;

    while ((kv = ht_next(&env->contents, &i)).key)
	ht_set(table, kv.key, kv.value);
}

/* Creates a new variable environment.
 * `temp' specifies whether the new environment is for temporary assignments.
 * The current environment will be the parent of the new environment. */
/* Don't forget to call `set_positional_parameters'! */
void open_new_environment(bool temp)
{
    environ_T *newenv = xmalloc(sizeof *newenv);

    newenv->parent = current_env;
    newenv->is_temporary = temp;
    ht_init(&newenv->contents, hashwcs, htwcscmp);
    for (size_t i = 0; i < PA_count; i++)
	newenv->paths[i] = NULL;
    current_env = newenv;
}

/* Destroys the current variable environment.
 * The parent of the current becomes the new current. */
void close_current_environment(void)
{
    environ_T *oldenv = current_env;

    assert(oldenv != first_env);
    current_env = oldenv->parent;
    ht_clear(&oldenv->contents, varkvfree_reexport);
    ht_destroy(&oldenv->contents);
    for (size_t i = 0; i < PA_count; i++)
	recfree((void **) oldenv->paths[i], free);
    free(oldenv);
}


/********** Getters **********/

/* line number of the currently executing command */
unsigned long current_lineno;

/* getter for $LINENO */
void lineno_getter(variable_T *var)
{
    assert((var->v_type & VF_MASK) == VF_SCALAR);
    free(var->v_value);
    var->v_value = malloc_wprintf(L"%lu", current_lineno);
    // variable_set(VAR_LINENO, var);
    if (var->v_type & VF_EXPORT)
	update_environment(L VAR_LINENO);
}

/* getter for $RANDOM */
void random_getter(variable_T *var)
{
    assert((var->v_type & VF_MASK) == VF_SCALAR);
    free(var->v_value);
    var->v_value = malloc_wprintf(L"%u", next_random());
    // variable_set(VAR_RANDOM, var);
    if (var->v_type & VF_EXPORT)
	update_environment(L VAR_RANDOM);
}

/* Returns a random number between 0 and 32767 using `rand'. */
unsigned next_random(void)
{
#if RAND_MAX == 32767
    return (unsigned) rand();
#elif RAND_MAX == 65535
    return (unsigned) rand() >> 1;
#elif RAND_MAX == 2147483647
    return (unsigned) rand() >> 16;
#elif RAND_MAX == 4294967295
    return (unsigned) rand() >> 17;
#else
    unsigned max, value;
    do {
	max   = RAND_MAX;
	value = rand();
	while (max > 65535) {
	    max   >>= 1;
	    value >>= 1;
	}
	if (max == 65535)
	    return value >> 1;
    } while (value > 32767);
    return value;
#endif
}


/********** Setter **********/

/* General callback function that is called after an assignment.
 * `var' is NULL when the variable is unset. */
void variable_set(const wchar_t *name, variable_T *var)
{
    switch (name[0]) {
    case L'C':
	if (wcscmp(name, L VAR_CDPATH) == 0)
	    reset_path(PA_CDPATH, var);
#if YASH_ENABLE_LINEEDIT
	if (wcscmp(name, L VAR_COLUMNS) == 0)
	    le_need_term_update = true;
#endif
	break;
    case L'L':
	if (wcscmp(name, L VAR_LANG) == 0 || wcsncmp(name, L"LC_", 3) == 0)
	    reset_locale(name);
#if YASH_ENABLE_LINEEDIT
	if (wcscmp(name, L VAR_LINES) == 0)
	    le_need_term_update = true;
#endif
	break;
    case L'P':
	if (wcscmp(name, L VAR_PATH) == 0) {
	    clear_cmdhash();
	    reset_path(PA_PATH, var);
	}
	break;
    case L'R':
	if (random_active && wcscmp(name, L VAR_RANDOM) == 0) {
	    random_active = false;
	    if (var && (var->v_type & VF_MASK) == VF_SCALAR && var->v_value) {
		unsigned long seed;
		if (xwcstoul(var->v_value, 0, &seed)) {
		    srand((unsigned) seed);
		    var->v_getter = random_getter;
		    random_active = true;
		}
	    }
	}
	break;
#if YASH_ENABLE_LINEEDIT
    case L'T':
	if (wcscmp(name, L VAR_TERM) == 0)
	    le_need_term_update = true;
	break;
#endif /* YASH_ENABLE_LINEEDIT */
    case L'Y':
	if (wcscmp(name, L VAR_YASH_LOADPATH) == 0)
	    reset_path(PA_LOADPATH, var);
	break;
    }
}


/********** Path Array Manipulation **********/

/* Splits the specified string at colons.
 * If `paths' is non-NULL, a newly-malloced NULL-terminated array of pointers to
 * newly-malloced multibyte strings is returned.
 * If `paths' is NULL, NULL is returned. */
char **decompose_paths(const wchar_t *paths)
{
    if (!paths)
	return NULL;

    plist_T list;
    pl_init(&list);

    const wchar_t *colon;
    while ((colon = wcschr(paths, L':')) != NULL) {
	add_to_list_no_dup(&list, malloc_wcsntombs(paths, colon - paths));
	paths = colon + 1;
    }
    add_to_list_no_dup(&list, malloc_wcstombs(paths));

    return (char **) pl_toary(&list);
}

/* Converts an array of wide strings into an newly-malloced array of multibyte
 * strings.
 * If `paths' is NULL, NULL is returned. */
char **convert_path_array(void **ary)
{
    if (!ary)
	return NULL;

    plist_T list;
    pl_init(&list);

    while (*ary) {
	add_to_list_no_dup(&list, malloc_wcstombs(*ary));
	ary++;
    }

    return (char **) pl_toary(&list);
}

/* If `s' is non-NULL and not contained in `list', add `s' to list.
 * Otherwise, frees `s'. */
void add_to_list_no_dup(plist_T *list, char *s)
{
    if (s) {
	for (size_t i = 0; i < list->length; i++) {
	    if (strcmp(s, list->contents[i]) == 0) {
		free(s);
		return;
	    }
	}
	pl_add(list, s);
    }
}

/* Reconstructs the path array of the specified variable in the environment.
 * `var' may be NULL. */
void reset_path(path_T name, variable_T *var)
{
    for (environ_T *env = current_env; env; env = env->parent) {
	recfree((void **) env->paths[name], free);

	variable_T *v = ht_get(&env->contents, path_variables[name]).value;
	if (v) {
	    switch (v->v_type & VF_MASK) {
		case VF_SCALAR:
		    env->paths[name] = decompose_paths(v->v_value);
		    break;
		case VF_ARRAY:
		    env->paths[name] = convert_path_array(v->v_vals);
		    break;
	    }
	    if (v == var)
		break;
	} else {
	    env->paths[name] = NULL;
	}
    }
}

/* Returns the path array of the specified variable.
 * The return value is NULL if the variable is not set.
 * The caller must not make any change to the returned array. */
char *const *get_path_array(path_T name)
{
    for (environ_T *env = current_env; env; env = env->parent)
	if (env->paths[name])
	    return env->paths[name];
    return NULL;
}


/********** Shell Functions **********/

/* type of functions */
struct function_T {
    vartype_T  f_type;  /* only VF_READONLY and VF_NODELETE are valid */
    command_T *f_body;  /* body of function */
};

/* Frees the specified function. */
void funcfree(function_T *f)
{
    if (f) {
	comsfree(f->f_body);
	free(f);
    }
}

/* Frees the specified key-value pair of a function name and a function. */
void funckvfree(kvpair_T kv)
{
    free(kv.key);
    funcfree(kv.value);
}

/* Defines function `name' as command `body'.
 * It is an error to re-define a readonly function.
 * Returns true iff successful. */
bool define_function(const wchar_t *name, command_T *body)
{
    function_T *f = ht_get(&functions, name).value;
    if (f != NULL && (f->f_type & VF_READONLY)) {
	xerror(0, Ngt("function `%ls' cannot be redefined "
		    "because it is read-only"), name);
	return false;
    }

    f = xmalloc(sizeof *f);
    f->f_type = 0;
    f->f_body = comsdup(body);
    if (shopt_hashondef)
	hash_all_commands_recursively(body);
    funckvfree(ht_set(&functions, xwcsdup(name), f));
    return true;
}

/* Gets the body of the function with the specified name.
 * Returns NULL if there is no such a function. */
command_T *get_function(const wchar_t *name)
{
    function_T *f = ht_get(&functions, name).value;
    if (f)
	return f->f_body;
    else
	return NULL;
}


/* Registers all the commands in the argument to the command hashtable. */
void hash_all_commands_recursively(const command_T *c)
{
    for (; c; c = c->next) {
	switch (c->c_type) {
	    case CT_SIMPLE:
		if (c->c_words)
		    tryhash_word_as_command(c->c_words[0]);
		break;
	    case CT_GROUP:
	    case CT_SUBSHELL:
		hash_all_commands_in_and_or(c->c_subcmds);
		break;
	    case CT_IF:
		hash_all_commands_in_if(c->c_ifcmds);
		break;
	    case CT_FOR:
		hash_all_commands_in_and_or(c->c_forcmds);
		break;
	    case CT_WHILE:
		hash_all_commands_in_and_or(c->c_whlcond);
		hash_all_commands_in_and_or(c->c_whlcmds);
		break;
	    case CT_CASE:
		hash_all_commands_in_case(c->c_casitems);
		break;
	    case CT_FUNCDEF:
		break;
	}
    }
}

void hash_all_commands_in_and_or(const and_or_T *ao)
{
    for (; ao; ao = ao->next) {
	for (pipeline_T *p = ao->ao_pipelines; p; p = p->next) {
	    hash_all_commands_recursively(p->pl_commands);
	}
    }
}

void hash_all_commands_in_if(const ifcommand_T *ic)
{
    for (; ic; ic = ic->next) {
	hash_all_commands_in_and_or(ic->ic_condition);
	hash_all_commands_in_and_or(ic->ic_commands);
    }
}

void hash_all_commands_in_case(const caseitem_T *ci)
{
    for (; ci; ci = ci->next) {
	hash_all_commands_in_and_or(ci->ci_commands);
    }
}

void tryhash_word_as_command(const wordunit_T *w)
{
    if (w && !w->next && w->wu_type == WT_STRING) {
	wchar_t *cmdname = unquote(w->wu_string);
	if (wcschr(cmdname, L'/') == NULL) {
	    char *mbsname = malloc_wcstombs(cmdname);
	    get_command_path(mbsname, false);
	    free(mbsname);
	}
	free(cmdname);
    }
}

#if YASH_ENABLE_LINEEDIT

/* Generates completion candidates for variable names matching the pattern. */
/* The prototype of this function is declared in "lineedit/complete.h". */
void generate_variable_candidates(const le_compopt_T *compopt)
{
    if (!(compopt->type & CGT_VARIABLE))
	return;

    le_compdebug("adding variable name candidates");
    if (!le_compile_cpatterns(compopt))
	return;

    size_t i = 0;
    kvpair_T kv;
    while ((kv = ht_next(&first_env->contents, &i)).key != NULL) {
	const wchar_t *name = kv.key;
	const variable_T *var = kv.value;
	switch (var->v_type & VF_MASK) {
	    case VF_SCALAR:
		if (!(compopt->type & CGT_SCALAR))
		    continue;
		break;
	    case VF_ARRAY:
		if (!(compopt->type & CGT_ARRAY))
		    continue;
		break;
	}
	if (name[0] != L'=' && le_wmatch_comppatterns(compopt, name))
	    le_new_candidate(CT_VAR, xwcsdup(name), NULL, compopt);
    }
}

/* Generates completion candidates for function names matching the pattern. */
/* The prototype of this function is declared in "lineedit/complete.h". */
void generate_function_candidates(const le_compopt_T *compopt)
{
    if (!(compopt->type & CGT_FUNCTION))
	return;

    le_compdebug("adding function name candidates");
    if (!le_compile_cpatterns(compopt))
	return;

    size_t i = 0;
    const wchar_t *name;
    while ((name = ht_next(&functions, &i).key) != NULL)
	if (le_wmatch_comppatterns(compopt, name))
	    le_new_candidate(CT_COMMAND, xwcsdup(name), NULL, compopt);
}

#endif /* YASH_ENABLE_LINEEDIT */


/********** Directory Stack **********/

#if YASH_ENABLE_DIRSTACK

/* Parses the specified string as the index of a directory stack entry.
 * The index string must start with the plus or minus sign.
 * If the corresponding entry is found, the entry is assigned to `*entryp' and
 * the index value is assigned to `*indexp'. If the index is out of range, it is
 * an error. If the string is not a valid integer, `indexstr' is assigned to
 * `*entryp' and SIZE_MAX is assigned to `*indexp'.
 * If the index denotes $PWD, the number of the entries in $DIRSTACK is assigned
 * to `*indexp'.
 * Returns true if successful (`*entryp' and `*indexp' are assigned).
 * Returns false on error, in which case an error message is printed iff
 * `printerror' is true. */
bool parse_dirstack_index(
	const wchar_t *restrict indexstr, size_t *restrict indexp,
	const wchar_t **restrict entryp, bool printerror)
{
    long num;

    if (indexstr[0] != L'-' && indexstr[0] != L'+')
	goto not_index;
    if (!xwcstol(indexstr, 10, &num))
	goto not_index;

    variable_T *var = search_variable(L VAR_DIRSTACK);
    if (!var || ((var->v_type & VF_MASK) != VF_ARRAY)) {
	if (num == 0)
	    goto return_pwd;
	if (printerror)
	    xerror(0, Ngt("the directory stack is empty"));
	return false;
    }
#if LONG_MAX > SIZE_MAX
    if (num > SIZE_MAX)
	goto out_of_range;
#endif
    if (indexstr[0] == L'+' && num >= 0) {
	if (num == 0) {
	    num = var->v_valc;
	    goto return_pwd;
	}
	if ((size_t) num > var->v_valc)
	    goto out_of_range;
	*indexp = var->v_valc - (size_t) num;
	*entryp = var->v_vals[*indexp];
	return true;
    } else if (indexstr[0] == L'-' && num <= 0) {
	if (num == LONG_MIN)
	    goto out_of_range;
	num = -num;
	assert(num >= 0);
	if ((size_t) num > var->v_valc)
	    goto out_of_range;
	if ((size_t) num == var->v_valc)
	    goto return_pwd;
	*indexp = (size_t) num;
	*entryp = var->v_vals[*indexp];
	return true;
    } else {
	goto not_index;
    }

    const wchar_t *pwd;
return_pwd:
    pwd = getvar(L VAR_PWD);
    if (!pwd) {
	if (printerror)
	    xerror(0, Ngt("$PWD is not set"));
	return false;
    }
    *entryp = pwd;
    *indexp = (size_t) num;
    return true;
not_index:
    *entryp = indexstr;
    *indexp = SIZE_MAX;
    return true;
out_of_range:
    if (printerror)
	xerror(0, Ngt("index %ls is out of range"), indexstr);
    return false;
}

#endif /* YASH_ENABLE_DIRSTACK */


/********** Builtins **********/

static void print_variable(
	const wchar_t *name, const variable_T *var,
	const wchar_t *argv0, bool readonly, bool export)
    __attribute__((nonnull));
static void print_function(
	const wchar_t *name, const function_T *func,
	const wchar_t *argv0, bool readonly)
    __attribute__((nonnull));
#if YASH_ENABLE_ARRAY
static void array_remove_elements(
	variable_T *array, size_t count, void *const *indexwcss)
    __attribute__((nonnull));
static int compare_long(const void *lp1, const void *lp2)
    __attribute__((nonnull,pure));
static void array_insert_elements(
	variable_T *array, size_t count, void *const *values)
    __attribute__((nonnull));
static void array_set_element(const wchar_t *name, variable_T *array,
	const wchar_t *indexword, const wchar_t *value)
    __attribute__((nonnull));
#endif /* YASH_ENABLE_ARRAY */
static bool unset_function(const wchar_t *name)
    __attribute__((nonnull));
static bool unset_variable(const wchar_t *name)
    __attribute__((nonnull));
static bool check_options(const wchar_t *options)
    __attribute__((nonnull,pure));
static bool set_optind(unsigned long optind, unsigned long optsubind);
static inline bool set_optarg(const wchar_t *value);
static bool set_to(const wchar_t *varname, wchar_t value)
    __attribute__((nonnull));
static bool read_with_prompt(xwcsbuf_T *buf, bool noescape)
    __attribute__((nonnull));
static void split_and_assign_array(const wchar_t *name, wchar_t *values,
	const wchar_t *ifs, bool raw)
    __attribute__((nonnull));

/* The "typeset" builtin, which accepts the following options:
 *  -f: affect functions rather than variables
 *  -g: global
 *  -p: print variables
 *  -r: make variables readonly
 *  -x: export variables
 *  -X: cancel exportation of variables
 * Equivalent builtins:
 *  export:   typeset -gx
 *  readonly: typeset -gr
 * The "set" builtin without any arguments is redirected to this builtin. */
int typeset_builtin(int argc, void **argv)
{
    static const struct xoption long_options[] = {
	{ L"functions", OPTARG_NONE, L'f', },
	{ L"global",    OPTARG_NONE, L'g', },
	{ L"print",     OPTARG_NONE, L'p', },
	{ L"readonly",  OPTARG_NONE, L'r', },
	{ L"export",    OPTARG_NONE, L'x', },
	{ L"unexport",  OPTARG_NONE, L'X', },
#if YASH_ENABLE_HELP
	{ L"help",      OPTARG_NONE, L'-', },
#endif
	{ NULL, 0, 0, },
    };

    wchar_t opt;
    bool funcs = false, global = false, print = false;
    bool readonly = false, export = false, unexport = false;

    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv, posixly_correct ? L"p" : L"fgprxX",
		    long_options, NULL))) {
	switch (opt) {
	    case L'f':  funcs    = true;  break;
	    case L'g':  global   = true;  break;
	    case L'p':  print    = true;  break;
	    case L'r':  readonly = true;  break;
	    case L'x':  export   = true;  break;
	    case L'X':  unexport = true;  break;
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:
		fprintf(stderr, gt(posixly_correct
			    ? Ngt("Usage:  %ls [-p] [name[=value]...]\n")
			    : Ngt("Usage:  %ls [-fgprxX] [name[=value]...]\n")),
			ARGV(0));
		SPECIAL_BI_ERROR;
		return Exit_ERROR;
	}
    }

    if (wcscmp(ARGV(0), L"export") == 0) {
	global = true;
	if (!unexport)
	    export = true;
    } else if (wcscmp(ARGV(0), L"readonly") == 0) {
	global = readonly = true;
    } else {
	assert(wcscmp(ARGV(0), L"typeset") == 0
	    || wcscmp(ARGV(0), L"set") == 0);
    }

    if (funcs && (export || unexport)) {
	xerror(0, Ngt("the -f option cannot be used with the -x or -X option"));
	SPECIAL_BI_ERROR;
	return Exit_ERROR;
    } else if (export && unexport) {
	xerror(0, Ngt("the -x and -X options cannot be used both at once"));
	SPECIAL_BI_ERROR;
	return Exit_ERROR;
    }

    if (xoptind == argc) {
	kvpair_T *kvs;
	size_t count;

	if (!funcs) {
	    /* print all variables */
	    hashtable_T table;

	    ht_init(&table, hashwcs, htwcscmp);
	    get_all_variables_rec(&table, current_env, global);
	    kvs = ht_tokvarray(&table);
	    count = table.count;
	    ht_destroy(&table);
	    qsort(kvs, count, sizeof *kvs, keywcscoll);
	    for (size_t i = 0; yash_error_message_count == 0 && i < count; i++)
		print_variable(
			kvs[i].key, kvs[i].value, ARGV(0), readonly, export);
	} else {
	    /* print all functions */
	    kvs = ht_tokvarray(&functions);
	    count = functions.count;
	    qsort(kvs, count, sizeof *kvs, keywcscoll);
	    for (size_t i = 0; yash_error_message_count == 0 && i < count; i++)
		print_function(kvs[i].key, kvs[i].value, ARGV(0), readonly);
	}
	free(kvs);
    } else {
	do {
	    wchar_t *arg = ARGV(xoptind);
	    wchar_t *wequal = wcschr(arg, L'=');
	    if (wequal)
		*wequal = L'\0';
	    if (funcs) {
		/* treat function */
		if (wequal) {
		    xerror(0, Ngt("cannot assign function"));
		    continue;
		}

		function_T *f = ht_get(&functions, arg).value;
		if (f) {
		    if (readonly)
			f->f_type |= VF_READONLY | VF_NODELETE;
		    if (print)
			print_function(arg, f, ARGV(0), readonly);
		} else {
		    xerror(0, Ngt("no such function `%ls'"), arg);
		}
	    } else if (wequal || !print) {
		/* create/assign variable */
		variable_T *var = global ? new_global(arg) : new_local(arg);
		vartype_T saveexport = var->v_type & VF_EXPORT;
		if (wequal) {
		    if (var->v_type & VF_READONLY) {
			xerror(0, Ngt("$%ls is read-only"), arg);
		    } else {
			varvaluefree(var);
			var->v_type = VF_SCALAR | (var->v_type & ~VF_MASK);
			var->v_value = xwcsdup(wequal + 1);
			var->v_getter = NULL;
		    }
		}
		if (readonly)
		    var->v_type |= VF_READONLY | VF_NODELETE;
		if (export)
		    var->v_type |= VF_EXPORT;
		else if (unexport)
		    var->v_type &= ~VF_EXPORT;
		variable_set(arg, var);
		if (saveexport != (var->v_type & VF_EXPORT)
			|| (wequal && (var->v_type & VF_EXPORT)))
		    update_environment(arg);
	    } else {
		/* print the variable */
		variable_T *var = search_variable(arg);
		if (var) {
		    print_variable(arg, var, ARGV(0), readonly, export);
		} else {
		    xerror(0, Ngt("no such variable $%ls"), arg);
		}
	    }
	} while (++xoptind < argc);
    }

    return (yash_error_message_count == 0) ? Exit_SUCCESS : Exit_FAILURE;
}

/* Prints the specified variable to the standard output.
 * This function does not print special variables whose name begins with an '='.
 * If `readonly'/`export' is true, the variable is printed only if it is
 * readonly/exported. The `name' is quoted if `is_name(name)' is not true.
 * An error message is printed to the standard error on error. */
void print_variable(
	const wchar_t *name, const variable_T *var,
	const wchar_t *argv0, bool readonly, bool export)
{
    wchar_t *qname = NULL;

    if (name[0] == L'=')
	return;
    if (readonly && !(var->v_type & VF_READONLY))
	return;
    if (export && !(var->v_type & VF_EXPORT))
	return;

    if (!is_name(name))
	name = qname = quote_sq(name);
    switch (var->v_type & VF_MASK) {
	case VF_SCALAR:  goto print_variable;
	case VF_ARRAY:   goto print_array;
    }

print_variable:;
    wchar_t *qvalue = var->v_value ? quote_sq(var->v_value) : NULL;
    const char *format;
    xstrbuf_T opts;
    switch (argv0[0]) {
	case L's':
	    assert(wcscmp(argv0, L"set") == 0);
	    if (!qname && qvalue)
		if (printf("%ls=%ls\n", name, qvalue) < 0)
		    xerror(errno, Ngt("cannot print to standard output"));
	    break;
	case L'e':
	case L'r':
	    assert(wcscmp(argv0, L"export") == 0
		    || wcscmp(argv0, L"readonly") == 0);
	    format = qvalue ? "%ls %ls=%ls\n" : "%ls %ls\n";
	    if (printf(format, argv0, name, qvalue) < 0)
		xerror(errno, Ngt("cannot print to standard output"));
	    break;
	case L't':
	    assert(wcscmp(argv0, L"typeset") == 0);
	    sb_init(&opts);
	    if (var->v_type & VF_EXPORT)
		sb_ccat(&opts, 'x');
	    if (var->v_type & VF_READONLY)
		sb_ccat(&opts, 'r');
	    if (opts.length > 0)
		sb_insert(&opts, 0, " -");
	    format = qvalue ? "%ls%s %ls=%ls\n" : "%ls%s %ls\n";
	    if (printf(format, argv0, opts.contents, name, qvalue) < 0)
		xerror(errno, Ngt("cannot print to standard output"));
	    sb_destroy(&opts);
	    break;
	default:
	    assert(false);
    }
    free(qvalue);
    free(qname);
    return;

print_array:
    clearerr(stdout);
    printf("%ls=(", name);
    for (void **values = var->v_vals; *values; values++) {
	wchar_t *qvalue = quote_sq(*values);
	printf("%ls", qvalue);
	free(qvalue);
	if (*(values + 1))
	    printf(" ");
    }
    printf(")\n");
    switch (argv0[0]) {
	case L'a':
	    assert(wcscmp(argv0, L"array") == 0);
	    break;
	case L's':
	    assert(wcscmp(argv0, L"set") == 0);
	    break;
	case L'e':
	case L'r':
	    assert(wcscmp(argv0, L"export") == 0
		    || wcscmp(argv0, L"readonly") == 0);
	    printf("%ls %ls\n", argv0, name);
	    break;
	case L't':
	    assert(wcscmp(argv0, L"typeset") == 0);
	    sb_init(&opts);
	    if (var->v_type & VF_EXPORT)
		sb_ccat(&opts, 'x');
	    if (var->v_type & VF_READONLY)
		sb_ccat(&opts, 'r');
	    if (opts.length > 0)
		sb_insert(&opts, 0, " -");
	    printf("%ls%s %ls\n", argv0, opts.contents, name);
	    sb_destroy(&opts);
	    break;
	default:
	    assert(false);
    }
    free(qname);
    if (ferror(stdout))
	xerror(0, Ngt("cannot print to standard output"));
}

/* Prints the specified function to the standard output.
 * If `readonly' is true, the function is printed only if it is readonly.
 * An error message is printed to the standard error if failed to print to the
 * standard output. */
void print_function(
	const wchar_t *name, const function_T *func,
	const wchar_t *argv0, bool readonly)
{
    if (readonly && !(func->f_type & VF_READONLY))
	return;

    wchar_t *qname = NULL;
    if (!is_name(name))
	name = qname = quote_sq(name);

    wchar_t *value = command_to_wcs(func->f_body, true);
    clearerr(stdout);
    if (qname == NULL)
	printf("%ls()\n%ls", name, value);
    else
	printf("function %ls()\n%ls", name, value);
    free(value);

    switch (argv0[0]) {
	case L'r':
	    assert(wcscmp(argv0, L"readonly") == 0);
	    if (func->f_type & VF_READONLY)
		printf("%ls -f %ls\n", argv0, name);
	    break;
	case L't':
	    assert(wcscmp(argv0, L"typeset") == 0);
	    if (func->f_type & VF_READONLY)
		printf("%ls -fr %ls\n", argv0, name);
	    break;
	default:
	    assert(false);
    }
    free(qname);
    if (ferror(stdout))
	xerror(0, Ngt("cannot print to standard output"));
}

#if YASH_ENABLE_HELP
const char *typeset_help[] = { Ngt(
"typeset, export, readonly - set or print variables\n"
), Ngt(
"\ttypeset  [-fgprxX] [name[=value]...]\n"
"\texport   [-prX]    [name[=value]...]\n"
"\treadonly [-fpxX]   [name[=value]...]\n"
), Ngt(
"For each operand of the form <name>, the variable of the specified name is\n"
"created if not yet created, without assigning any value. If the -p (--print)\n"
"option is specified, the current value and the attributes of the variable\n"
"are printed instead of creating the variable.\n"
), Ngt(
"For each operand of the form <name=value>, the value is assigned to the\n"
"specified variable. The variable is created if not yet created.\n"
"If no operands are given, all existing variables are printed.\n"
), (
"\n"
), Ngt(
"By default, the typeset built-in affects local variables only. To create a\n"
"global variable inside a function, the -g (--global) option can be used.\n"
), Ngt(
"The -r (--readonly) option makes the specified variables/functions read-\n"
"only.\n"
), Ngt(
"The -x (--export) option makes the variables exported to external commands.\n"
), Ngt(
"The -X (--unexport) option undoes the exportation.\n"
), Ngt(
"The -f (--functions) option can be used to affect functions instead of\n"
"variables. Functions cannot be assigned or exported with the typeset\n"
"built-in: the -f option can be used only together with the -r or -p option\n"
"to make functions read-only or print them.\n"
), (
"\n"
), Ngt(
"`export' is equivalent to `typeset -gx'.\n"
"`readonly' is equivalent to `typeset -gr'.\n"
"Note that the typeset built-in is unavailable in the POSIXly correct mode.\n"
), NULL };
#endif /* YASH_ENABLE_HELP */

#if YASH_ENABLE_ARRAY

/* The "array" builtin, which accepts the following options:
 *  -d: delete an array element
 *  -i: insert an array element
 *  -s: set an array element value */
int array_builtin(int argc, void **argv)
{
    static const struct xoption long_options[] = {
	{ L"delete", OPTARG_NONE, L'd', },
	{ L"insert", OPTARG_NONE, L'i', },
	{ L"set",    OPTARG_NONE, L's', },
#if YASH_ENABLE_HELP
	{ L"help",   OPTARG_NONE, L'-', },
#endif
	{ NULL, 0, 0, },
    };

    enum {
	delete = 1 << 0,
	insert = 1 << 1,
	set    = 1 << 2,
    } options = 0;

    wchar_t opt;
    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv, L"-dis", long_options, NULL))) {
	switch (opt) {
	    case L'd':  options |= delete;  break;
	    case L'i':  options |= insert;  break;
	    case L's':  options |= set;     break;
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:  print_usage:
		fprintf(stderr, gt("Usage:  array [name [value...]]\n"
		                   "        array -d name [index...]\n"
				   "        array -i name index [value...]\n"
				   "        array -s name index value\n"));
		return Exit_ERROR;
	}
    }
    if (options && (options & (options - 1))) {
	xerror(0, Ngt("more than one option cannot be used at once"));
	return Exit_ERROR;
    }

    if (xoptind == argc) {
	/* print all arrays */
	if (options != 0)
	    goto print_usage;

	hashtable_T table;
	ht_init(&table, hashwcs, htwcscmp);
	get_all_variables_rec(&table, current_env, true);

	kvpair_T *kvs = ht_tokvarray(&table);
	size_t count = table.count;
	ht_destroy(&table);
	qsort(kvs, count, sizeof *kvs, keywcscoll);
	for (size_t i = 0; yash_error_message_count == 0 && i < count; i++)
	    if ((((variable_T *) kvs[i].value)->v_type & VF_MASK) == VF_ARRAY)
		print_variable(kvs[i].key, kvs[i].value, ARGV(0), false, false);
	free(kvs);
    } else {
	const wchar_t *name = ARGV(xoptind++);
	if (wcschr(name, L'=')) {
	    xerror(0, Ngt("`%ls' is not a valid array name"), name);
	    return Exit_FAILURE;
	}

	if (options == 0) {
	    set_array(name, argc - xoptind,
		    duparray(argv + xoptind, copyaswcs), SCOPE_GLOBAL);
	} else {
	    variable_T *array = search_array_and_check_if_changeable(name);
	    if (!array)
		return Exit_FAILURE;
	    switch (options) {
		case delete:
		    array_remove_elements(
			    array, argc - xoptind, argv + xoptind);
		    break;
		case insert:
		    if (xoptind == argc)
			goto print_usage;
		    array_insert_elements(
			    array, argc - xoptind, argv + xoptind);
		    break;
		case set:
		    if (xoptind + 2 != argc)
			goto print_usage;
		    array_set_element(
			    name, array, ARGV(xoptind), ARGV(xoptind + 1));
		    break;
		default:
		    assert(false);
	    }
	}
    }
    return (yash_error_message_count == 0) ? Exit_SUCCESS : Exit_FAILURE;
}

#if LONG_MAX < SIZE_MAX
# define LONG_LT_SIZE(longvalue,sizevalue) \
    ((size_t) (longvalue) < (sizevalue))
#else
# define LONG_LT_SIZE(longvalue,sizevalue) \
    ((longvalue) < (long) (sizevalue))
#endif

/* Removes elements from `array'.
 * `indexwcss' is an NULL-terminated array of pointers to wide strings,
 * which are parsed as indices of elements to be removed.
 * `count' is the number of elements in `indexwcss'.
 * An error message is printed to the standard error on error. */
void array_remove_elements(
	variable_T *array, size_t count, void *const *indexwcss)
{
    long indices[count], lastindex;
    plist_T list;

    assert((array->v_type & VF_MASK) == VF_ARRAY);

    /* convert all the strings into long integers */
    for (size_t i = 0; i < count; i++) {
	const wchar_t *indexwcs = indexwcss[i];
	if (!xwcstol(indexwcs, 10, &indices[i])) {
	    xerror(errno, Ngt("`%ls' is not a valid integer"), indexwcs);
	    return;
	}
    }

    /* sort all the indices and remove elements in descending order so that
     * an earlier removal does not affect the indices for later removals. */
    if (count > 1)
	qsort(indices, count, sizeof *indices, compare_long);

    pl_initwith(&list, array->v_vals, array->v_valc);
    lastindex = LONG_MIN;
    for (size_t i = count; i-- != 0; ) {
	long index = indices[i];
	if (index == lastindex)
	    continue;
	if (index > 0) {
	    if (LONG_LT_SIZE(index - 1, list.length)) {
		free(list.contents[index - 1]);
		pl_remove(&list, index - 1, 1);
	    }
	} else if (index < 0) {
	    index += array->v_valc;
	    if (index >= 0) {
		assert(LONG_LT_SIZE(index, list.length));
		free(list.contents[index]);
		pl_remove(&list, index, 1);
	    }
	}
	lastindex = index;
    }
    array->v_valc = list.length;
    array->v_vals = pl_toary(&list);
}

int compare_long(const void *lp1, const void *lp2)
{
    long l1 = *(const long *) lp1, l2 = *(const long *) lp2;
    return l1 == l2 ? 0 : l1 < l2 ? -1 : 1;
}

/* Inserts the specified elements into the specified array.
 * `values' is an NULL-terminated array of pointers to wide strings.
 * The first string in `values' is parsed as the integer index that specifies
 * where to insert the elements. The other strings are inserted to the array.
 * `count' is the number of strings in `values' including the first index
 * string.
 * An error message is printed to the standard error on error. */
void array_insert_elements(
	variable_T *array, size_t count, void *const *values)
{
    long index;
    plist_T list;

    assert((array->v_type & VF_MASK) == VF_ARRAY);
    assert(count > 0);
    assert(values[0] != NULL);
    {
	const wchar_t *indexword = *values;
	if (!xwcstol(indexword, 10, &index)) {
	    xerror(errno, Ngt("`%ls' is not a valid integer"), indexword);
	    return;
	}
    }
    count--, values++;
    assert(plcount(values) == count);

    size_t uindex;
    if (index < 0) {
	index += array->v_valc + 1;
	if (index < 0)
	    index = 0;
    }
    uindex = LONG_LT_SIZE(index, array->v_valc) ? (size_t)index : array->v_valc;

    pl_initwith(&list, array->v_vals, array->v_valc);
    pl_insert(&list, uindex, values);
    for (size_t i = 0; i < count; i++)
	list.contents[uindex + i] = xwcsdup(list.contents[uindex + i]);
    array->v_valc = list.length;
    array->v_vals = pl_toary(&list);
}

/* Sets the value of the specified element of the array.
 * `name' is the name of the array variable.
 * `indexword' is parsed as the integer index of the element.
 * An error message is printed to the standard error on error. */
void array_set_element(const wchar_t *name, variable_T *array,
	const wchar_t *indexword, const wchar_t *value)
{
    assert((array->v_type & VF_MASK) == VF_ARRAY);

    long index;
    if (!xwcstol(indexword, 10, &index)) {
	xerror(errno, Ngt("`%ls' is not a valid integer"), indexword);
	return;
    }

    size_t uindex;
    if (index < 0) {
	index += array->v_valc;
	if (index < 0)
	    goto invalid_index;
	assert(LONG_LT_SIZE(index, array->v_valc));
	uindex = (size_t) index;
    } else if (index > 0) {
	if (!LONG_LT_SIZE(index - 1, array->v_valc))
	    goto invalid_index;
	uindex = (size_t) index - 1;
    } else {
	goto invalid_index;
    }
    assert(uindex < array->v_valc);
    free(array->v_vals[uindex]);
    array->v_vals[uindex] = xwcsdup(value);
    return;

invalid_index:
    xerror(0, Ngt("index %ls is out of range "
		"(the actual size of array $%ls is %zu)"),
	    indexword, name, array->v_valc);
}

#if YASH_ENABLE_HELP
const char *array_help[] = { Ngt(
"array - manipulate an array\n"
), Ngt(
"\tarray\n"
"\tarray name [value...]\n"
"\tarray -d name [index...]\n"
"\tarray -i name index [value...]\n"
"\tarray -s name index value\n"
), Ngt(
"The first form (without arguments) prints all existing arrays.\n"
), Ngt(
"The second form sets the values of the array of the specified <name>. This\n"
"is equivalent to an assignment of the form `name=(values)'.\n"
), Ngt(
"The third form (with the -d (--delete) option) removes the elements of the\n"
"specified <index>es from the array.\n"
), Ngt(
"The fourth form (with the -i (--insert) option) inserts elements after the\n"
"element of the specified <index> in the array. If the index is zero, the\n"
"elements are inserted at the head of the array.\n"
), Ngt(
"The fifth form (with the -s (--set) option) sets the value of the specified\n"
"single element.\n"
), NULL };
#endif /* YASH_ENABLE_HELP */

#endif /* YASH_ENABLE_ARRAY */

/* The "unset" builtin, which accepts the following options:
 *  -f: deletes functions
 *  -v: deletes variables (default) */
int unset_builtin(int argc, void **argv)
{
    static const struct xoption long_options[] = {
	{ L"functions", OPTARG_NONE, L'f', },
	{ L"variables", OPTARG_NONE, L'v', },
#if YASH_ENABLE_HELP
	{ L"help",      OPTARG_NONE, L'-', },
#endif
	{ NULL, 0, 0, },
    };

    wchar_t opt;
    bool funcs = false;

    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv, L"fv", long_options, NULL))) {
	switch (opt) {
	    case L'f':  funcs = true;   break;
	    case L'v':  funcs = false;  break;
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:
		fprintf(stderr, gt("Usage:  unset [-fv] name...\n"));
		SPECIAL_BI_ERROR;
		return Exit_ERROR;
	}
    }

    for (; xoptind < argc; xoptind++) {
	const wchar_t *name = ARGV(xoptind);
	if (wcschr(name, L'=')) {
	    xerror(0, Ngt("`%ls' is not a valid variable name"), name);
	    continue;
	}
	if (funcs)
	    unset_function(name);
	else
	    unset_variable(name);
    }

    return (yash_error_message_count == 0) ? Exit_SUCCESS : Exit_FAILURE;
}

/* Unsets the specified function.
 * On error, an error message is printed to the standard error and TRUE is
 * returned. */
bool unset_function(const wchar_t *name)
{
    kvpair_T kv = ht_remove(&functions, name);
    function_T *f = kv.value;
    if (f) {
	if (!(f->f_type & VF_NODELETE)) {
	    funckvfree(kv);
	} else {
	    xerror(0, Ngt("function `%ls' is read-only"), name);
	    ht_set(&functions, kv.key, kv.value);
	    return true;
	}
    }
    return false;
}

/* Unsets the specified variable.
 * On error, an error message is printed to the standard error and TRUE is
 * returned. */
bool unset_variable(const wchar_t *name)
{
    for (environ_T *env = current_env; env; env = env->parent) {
	kvpair_T kv = ht_remove(&env->contents, name);
	variable_T *var = kv.value;
	if (var) {
	    if (!(var->v_type & VF_NODELETE)) {
		bool ue = var->v_type & VF_EXPORT;
		varkvfree(kv);
		variable_set(name, NULL);
		if (ue)
		    update_environment(name);
		return false;
	    } else {
		xerror(0, Ngt("$%ls is read-only"), name);
		ht_set(&env->contents, kv.key, kv.value);
		return true;
	    }
	}
    }
    return false;
}

#if YASH_ENABLE_HELP
const char *unset_help[] = { Ngt(
"unset - remove variables or functions\n"
), Ngt(
"\tunset [-fv] <name>...\n"
), Ngt(
"The unset built-in removes the specified variables or functions.\n"
"When the -f (--functions) option is specified, this built-in removes\n"
"functions. When the -v (--variables) option is specified, this built-in\n"
"removes variables.\n"
), Ngt(
"-f and -v are mutually exclusive: only the last specified one is effective.\n"
"If neither is specified, -v is the default.\n"
), NULL };
#endif

/* The "shift" builtin */
int shift_builtin(int argc, void **argv)
{
    wchar_t opt;

    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv, L"", help_option, NULL))) {
	switch (opt) {
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:  print_usage:
		fprintf(stderr, gt("Usage:  shift [n]\n"));
		SPECIAL_BI_ERROR;
		return Exit_ERROR;
	}
    }
    if (argc - xoptind > 1)
	goto print_usage;

    size_t scount;
    if (xoptind < argc) {
	long count;
	if (!xwcstol(ARGV(xoptind), 10, &count)) {
	    xerror(errno, Ngt("`%ls' is not a valid integer"), ARGV(xoptind));
	    SPECIAL_BI_ERROR;
	    return Exit_ERROR;
	} else if (count < 0) {
	    xerror(0, Ngt("%ls: the operand value must not be negative"),
		    ARGV(xoptind));
	    SPECIAL_BI_ERROR;
	    return Exit_ERROR;
	}
#if LONG_MAX <= SIZE_MAX
	scount = (size_t) count;
#else
	scount = (count > (long) SIZE_MAX) ? SIZE_MAX : (size_t) count;
#endif
    } else {
	scount = 1;
    }

    variable_T *var = search_variable(L VAR_positional);
    assert(var != NULL && (var->v_type & VF_MASK) == VF_ARRAY);
    if (scount > var->v_valc) {
	xerror(0, Ngt("%zu: cannot shift so many ($# = %zu)"),
		scount, var->v_valc);
	return Exit_FAILURE;
    }
    for (size_t i = 0; i < scount; i++)
	free(var->v_vals[i]);
    var->v_valc -= scount;
    memmove(var->v_vals, var->v_vals + scount,
	    (var->v_valc + 1) * sizeof *var->v_vals);
    return Exit_SUCCESS;
}

#if YASH_ENABLE_HELP
const char *shift_help[] = { Ngt(
"shift - remove some positional parameters\n"
), Ngt(
"\tshift [n]\n"
), Ngt(
"The shift built-in removes the first <n> positional parameters.\n"
"If <n> is not specified, it defaults to 1.\n"
"<n> must be a non-negative integer that is not greater than $#.\n"
), NULL };
#endif

/* The "getopts" builtin */
int getopts_builtin(int argc, void **argv)
{
    wchar_t opt;
    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv, L"+", help_option, NULL))) {
	switch (opt) {
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:
		goto print_usage;
	}
    }
    if (xoptind + 2 > argc)
	goto print_usage;

    const wchar_t *options = ARGV(xoptind++);
    const wchar_t *varname = ARGV(xoptind++);
    void *const *args;
    unsigned long optind, optsubind;
    const wchar_t *arg, *optp;
    wchar_t optchar;

    if (wcschr(varname, L'=')) {
	xerror(0, Ngt("`%ls' is not a valid variable name"), varname);
	return Exit_FAILURE;
    } else if (!check_options(options)) {
	xerror(0, Ngt("`%ls' is not a valid option specification"), options);
	return Exit_FAILURE;
    }

    /* Parse $OPTIND */
    {
	const wchar_t *varoptind = getvar(L VAR_OPTIND);
	wchar_t *endp;
	if (!varoptind || !varoptind[0])
	    goto optind_invalid;
	errno = 0;
	optind = wcstoul(varoptind, &endp, 10);
	if (errno || varoptind == endp)
	    goto optind_invalid;
	optind -= 1;

	if (*endp == L':') {
	    endp++;
	    if (!xwcstoul(endp, 10, &optsubind) || optsubind == 0)
		goto optind_invalid;
	} else {
	    optsubind = 1;
	}
    }

    if (xoptind < argc) {
	if (optind >= (unsigned long) (argc - xoptind))
	    goto no_more_options;
	args = argv + xoptind;
    } else {
	variable_T *var = search_variable(L VAR_positional);
	assert(var != NULL && (var->v_type & VF_MASK) == VF_ARRAY);
	if (optind >= var->v_valc)
	    goto no_more_options;
	args = var->v_vals;
    }

#define TRY(exp)  do { if (!(exp)) return Exit_FAILURE; } while (0)
parse_arg:
    arg = args[optind];
    if (arg == NULL || arg[0] != L'-' || arg[1] == L'\0') {
	goto no_more_options;
    } else if (arg[1] == L'-' && arg[2] == L'\0') {  /* arg == "--" */
	optind++;
	goto no_more_options;
    } else if (xwcsnlen(arg, optsubind + 1) <= optsubind) {
	optind++, optsubind = 1;
	goto parse_arg;
    }

    optchar = arg[optsubind++];
    assert(optchar != L'\0');
    if (optchar == L':' || !(optp = wcschr(options, optchar))) {
	/* invalid option */
	TRY(set_to(varname, L'?'));
	if (options[0] == L':') {
	    TRY(set_optarg((wchar_t []) { optchar, L'\0' }));
	} else {
	    fprintf(stderr, gt("%ls: `-%lc' is not a valid option\n"),
		    command_name, (wint_t) optchar);
	    TRY(!unset_variable(L VAR_OPTARG));
	}
    } else {
	/* valid option */
	if (optp[1] == L':') {
	    /* option with an argument */
	    const wchar_t *optarg = arg + optsubind;
	    optsubind = 1;
	    if (optarg[0] != L'\0') {
		optind++;
	    } else {
		optind++;
		optarg = args[optind];
		optind++;
		if (optarg == NULL) {
		    /* argument is missing */
		    if (options[0] == L':') {
			TRY(set_to(varname, L':'));
			TRY(set_optarg((wchar_t []) { optchar, L'\0' }));
		    } else {
			fprintf(stderr,
			    gt("%ls: the -%lc option's argument is missing\n"),
			    command_name, (wint_t) optchar);
			TRY(set_to(varname, L'?'));
			TRY(!unset_variable(L VAR_OPTARG));
		    }
		    goto finish;
		}
	    }
	    TRY(set_optarg(optarg));
	} else {
	    /* option without an argument */
	    TRY(!unset_variable(L VAR_OPTARG));
	}
	TRY(set_to(varname, optchar));
    }
finish:
    TRY(set_optind(optind, optsubind));
    return Exit_SUCCESS;
#undef TRY

no_more_options:
    set_optind(optind, 0);
    set_to(varname, L'?');
    unset_variable(L VAR_OPTARG);
    return Exit_FAILURE;
optind_invalid:
    xerror(0, Ngt("$OPTIND has an invalid value"));
    return Exit_FAILURE;
print_usage:
    fprintf(stderr, gt("Usage:  getopts options var [arg...]\n"));
    return Exit_ERROR;
}

/* Checks if the `options' is valid. Returns true iff ok. */
bool check_options(const wchar_t *options)
{
    if (*options == L':')
	options++;
    for (;;) switch (*options) {
	case L'\0':
	    return true;
	case L':':
	    return false;
	case L'?':
	    if (posixly_correct)
		return false;
	    /* falls thru! */
	default:
	    if (posixly_correct && !iswalpha(*options))
		return false;
	    options++;
	    if (*options == L':')
		options++;
	    continue;
    }
}

/* Sets $OPTIND to `optind' plus 1, followed by `optsubind' (if > 1). */
bool set_optind(unsigned long optind, unsigned long optsubind)
{
    wchar_t *value = malloc_wprintf(
	    optsubind > 1 ? L"%lu:%lu" : L"%lu",
	    optind + 1, optsubind);
    return set_variable(L VAR_OPTIND, value, SCOPE_GLOBAL, shopt_allexport);
}

/* Sets $OPTARG to `value'. */
bool set_optarg(const wchar_t *value)
{
    return set_variable(L VAR_OPTARG, xwcsdup(value),
	    SCOPE_GLOBAL, shopt_allexport);
}

/* Sets the specified variable to the single character `value'. */
bool set_to(const wchar_t *varname, wchar_t value)
{
    wchar_t *v = xmalloc(sizeof *v * 2);
    v[0] = value;
    v[1] = L'\0';
    return set_variable(varname, v, SCOPE_GLOBAL, shopt_allexport);
}

#if YASH_ENABLE_HELP
const char *getopts_help[] = { Ngt(
"getopts - parse command options\n"
), Ngt(
"\tgetopts options var [arg...]\n"
), Ngt(
"The getopts built-in parses <options> that appear in <arg>s. Each time\n"
"getopts is invoked, one option is parsed and the option character is\n"
"assigned to variable <var>.\n"
), Ngt(
"String <options> is a list of option characters that can be accepted by the\n"
"parser. In <options>, an option that takes an argument can be specified as\n"
"a character followed by a colon.\n"
"For example, if you want the -a, -b and -c options to be parsed and the -b\n"
"option takes an argument, then <options> should be `ab:c'.\n"
), Ngt(
"Argument <var> is the name of a variable to which the parsed option\n"
"character is assigned. When an option specified in <options> is parsed, the\n"
"option character is assigned to variable <var>. Otherwise, `?' is assigned\n"
"to <var>.\n"
), Ngt(
"Arguments <arg>s are the strings to parse. If no <arg>s are given, the\n"
"current positional parameters are parsed.\n"
), (
"\n"
), Ngt(
"When an option that takes an argument is parsed, the option's argument is\n"
"assigned to $OPTARG.\n"
), Ngt(
"When an option that is not specified in <options> is found or when an\n"
"option's argument is missing, the result depends on the first character of\n"
"<options>: If <options> starts with a colon, the option character is\n"
"assigned to $OPTARG and variable <var> is set to `?' (when the option is not\n"
"in <options>) or `:' (when the option's argument is missing). Otherwise,\n"
"variable <var> is set to `?', $OPTARG is unset, and an error message is\n"
"printed.\n"
), (
"\n"
), Ngt(
"If an option is found, whether or not it is specified in <options>, the exit\n"
"status is zero. If there is no more option to parse, the exit status is\n"
"non-zero and $OPTIND is updated so that the $OPTIND'th argument of <arg>s is\n"
"the first operand (non-option argument). If there are no operands, $OPTIND\n"
"will be the number of <arg>s plus one.\n"
), Ngt(
"When this command is invoked for the first time, $OPTIND must be `1', which\n"
"is the default value of $OPTIND. Until all the options are parsed, you must\n"
"not change the value of $OPTIND and the getopts built-in must be invoked\n"
"with the same arguments. Reset $OPTIND to `1' and then getopts can be used\n"
"with another set of <options>, <var> and <arg>s.\n"
), NULL };
#endif /* YASH_ENABLE_HELP */

/* The "read" builtin, which accepts the following options:
 *  -A: assign values to array
 *  -r: don't treat backslashes specially
 */
int read_builtin(int argc, void **argv)
{
    static const struct xoption long_options[] = {
	{ L"array",    OPTARG_NONE, L'A', },
	{ L"raw-mode", OPTARG_NONE, L'r', },
#if YASH_ENABLE_HELP
	{ L"help",     OPTARG_NONE, L'-', },
#endif
	{ NULL, 0, 0, },
    };

    wchar_t opt;
    bool array = false, raw = false;

    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv,
		    posixly_correct ? L"r" : L"Ar",
		    long_options, NULL))) {
	switch (opt) {
	    case L'A':  array = true;  break;
	    case L'r':  raw   = true;  break;
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:
		goto print_usage;
	}
    }
    if (xoptind == argc)
	goto print_usage;

    /* check if the identifiers are valid */
    for (int i = xoptind; i < argc; i++) {
	if (wcschr(ARGV(i), L'=')) {
	    xerror(0, Ngt("`%ls' is not a valid variable name"), ARGV(i));
	    return Exit_FAILURE;
	}
    }

    bool eof = false;
    xwcsbuf_T buf;

    /* read input and remove trailing newline */
    wb_init(&buf);
    if (!read_with_prompt(&buf, raw)) {
	wb_destroy(&buf);
	return Exit_FAILURE;
    }
    if (buf.length > 0 && buf.contents[buf.length - 1] == L'\n') {
	wb_remove(&buf, buf.length - 1, 1);
    } else {
	/* no newline means the EOF was encountered */
	eof = true;
    }

    const wchar_t *s = buf.contents;
    const wchar_t *ifs = getvar(L VAR_IFS);
    plist_T list;

    /* split fields */
    pl_init(&list);
    for (int i = xoptind; i < argc - 1; i++)
	pl_add(&list, split_next_field(&s, ifs, raw));
    pl_add(&list, (raw || array) ? xwcsdup(s) : unescape(s));
    wb_destroy(&buf);

    /* assign variables */
    assert(xoptind + list.length == (size_t) argc);
    for (size_t i = 0; i < list.length; i++) {
	const wchar_t *name = ARGV(xoptind + i);
	if (i + 1 == list.length)
	    trim_trailing_ifsws(list.contents[i], ifs);
	if (!array || i + 1 < list.length)
	    set_variable(name, list.contents[i], SCOPE_GLOBAL, shopt_allexport);
	else
	    split_and_assign_array(name, list.contents[i], ifs, raw);
    }

    pl_destroy(&list);
    return (!eof && yash_error_message_count == 0)
	    ? Exit_SUCCESS : Exit_FAILURE;

print_usage:
    if (posixly_correct)
	fprintf(stderr, gt("Usage:  read [-r] var...\n"));
    else
	fprintf(stderr, gt("Usage:  read [-Ar] var...\n"));
    return Exit_ERROR;
}

/* Reads input from the standard input and, if `noescape' is false, remove line
 * continuations.
 * The trailing newline and all other backslashes are not removed. */
bool read_with_prompt(xwcsbuf_T *buf, bool noescape)
{
    bool first = true;
    size_t index;

read_input:
    index = buf->length;
    if (is_interactive_now && isatty(STDIN_FILENO)) {
	/* if the shell is interactive and the input is from the terminal,
	 * then print a prompt and try to use line-editing. */

	struct promptset_T prompt;
	if (first) {
	    prompt.main   = xwcsdup(L"");
	    prompt.right  = xwcsdup(L"");
	    prompt.styler = xwcsdup(L"");
	} else {
	    prompt = get_prompt(2);
	}

#if YASH_ENABLE_LINEEDIT
	if (shopt_lineedit != shopt_nolineedit) {
	    wchar_t *line;
	    inputresult_T result = le_readline(prompt, &line);

	    if (result != INPUT_ERROR) {
		free_prompt(prompt);
		switch (result) {
		    case INPUT_OK:
			wb_catfree(buf, line);
			/* falls thru! */
		    case INPUT_EOF:
			goto done;
		    case INPUT_INTERRUPTED:
			set_interrupted();
			return false;
		    case INPUT_ERROR:
			assert(false);
		}
	    }
	}
#endif /* YASH_ENABLE_LINEEDIT */

	inputresult_T result2;
	print_prompt(prompt.main);
	print_prompt(prompt.styler);
	result2 = read_input(buf, stdin_input_file_info, false);
	print_prompt(PROMPT_RESET);
	free_prompt(prompt);
	if (result2 == INPUT_ERROR)
	    return false;
    } else {
	if (read_input(buf, stdin_input_file_info, false) == INPUT_ERROR)
	    return false;
    }

#if YASH_ENABLE_LINEEDIT
done:
#endif
    first = false;
    if (!noescape) {
	/* treat escapes */
	while (index < buf->length) {
	    if (buf->contents[index] == L'\\') {
		if (buf->contents[index + 1] == L'\n') {
		    wb_remove(buf, index, 2);
		    if (index >= buf->length)
			goto read_input;
		    else
			continue;
		} else {
		    index += 2;
		    continue;
		}
	    }
	    index++;
	}
    }

    return true;
}

/* Word-splits `values' and assigns them to the array named `name'.
 * `values' is freed in this function. */
void split_and_assign_array(const wchar_t *name, wchar_t *values,
	const wchar_t *ifs, bool raw)
{
    plist_T list;

    pl_init(&list);
    if (values[0] != L'\0') {
	const wchar_t *v = values;
	while (*v != L'\0')
	    pl_add(&list, split_next_field(&v, ifs, raw));

	if (list.length > 0
		&& ((wchar_t *) list.contents[list.length - 1])[0] == L'\0') {
	    free(list.contents[list.length - 1]);
	    pl_remove(&list, list.length - 1, 1);
	}
    }

    set_array(name, list.length, pl_toary(&list), SCOPE_GLOBAL);

    free(values);
}

#if YASH_ENABLE_HELP
const char *read_help[] = { Ngt(
"read - read a line from the standard input\n"
), Ngt(
"\tread [-Ar] var...\n"
), Ngt(
"The read built-in reads a line from the standard input and splits it into\n"
"words using $IFS as separators. The words are assigned to variables <var>s:\n"
"the first word is assigned to the first <var>, the second word to the second\n"
"<var>, and so on. If <var>s are fewer than the words, the leftover words are\n"
"not split and assigned to the last <var> at once. If the words are fewer\n"
"than <var>s, the leftover <var>s are set to empty strings.\n"
), Ngt(
"If the -r (--raw-mode) option is not specified, backslashes in the input are\n"
"considered to be an escape character.\n"
), Ngt(
"If the -A (--array) option is specified, the leftover words are assigned to\n"
"the array whose name is the last <var>.\n"
"The -A option is not available in the POSIXly correct mode.\n"
), NULL };
#endif

#if !YASH_ENABLE_DIRSTACK

/* options for the "pushd" builtins */
static const struct xoption pushd_options[] = {
    { L"default-directory", OPTARG_REQUIRED, L'd', },
    { L"logical",           OPTARG_NONE,     L'L', },
    { L"physical",          OPTARG_NONE,     L'P', },
#if YASH_ENABLE_HELP
    { L"help",              OPTARG_NONE,     L'-', },
#endif
    { NULL, 0, 0, },
};

/* options for the "cd" and "pwd" builtins */
const struct xoption *const cd_options  = pushd_options;
const struct xoption *const pwd_options = pushd_options + 1;

#else /* YASH_ENABLE_DIRSTACK */

/* options for the "pushd" builtins */
static const struct xoption pushd_options[] = {
    { L"remove-duplicates", OPTARG_NONE,     L'D', },
    { L"default-directory", OPTARG_REQUIRED, L'd', },
    { L"logical",           OPTARG_NONE,     L'L', },
    { L"physical",          OPTARG_NONE,     L'P', },
#if YASH_ENABLE_HELP
    { L"help",              OPTARG_NONE,     L'-', },
#endif
    { NULL, 0, 0, },
};

/* options for the "cd" and "pwd" builtins */
const struct xoption *const cd_options  = pushd_options + 1;
const struct xoption *const pwd_options = pushd_options + 2;

static variable_T *get_dirstack(void);
static void push_dirstack(variable_T *var, wchar_t *value)
    __attribute__((nonnull));
static void remove_dirstack_entry_at(variable_T *var, size_t index)
    __attribute__((nonnull));
static void remove_dirstack_dups(variable_T *var)
    __attribute__((nonnull));

/* The "pushd" builtin.
 *  -L: don't resolve symlinks (default)
 *  -P: resolve symlinks
 *  --default-directory=<dir>: go to <dir> when the operand is missing
 *  --remove-duplicates: remove duplicate entries in the directory stack.
 * -L and -P are mutually exclusive: the one specified last is used. */
int pushd_builtin(int argc __attribute__((unused)), void **argv)
{
    bool logical = true, remove_dups = false;
    const wchar_t *newpwd = L"+1";

    wchar_t opt;
    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv, L"-LP", pushd_options, NULL))) {
	switch (opt) {
	    case L'L':  logical = true;       break;
	    case L'P':  logical = false;      break;
	    case L'd':  newpwd = xoptarg;     break;
	    case L'D':  remove_dups = true;   break;
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:  print_usage:
		fprintf(stderr, gt("Usage:  %ls [-L|-P] [dir]\n"), L"pushd");
		return Exit_ERROR;
	}
    }

    const wchar_t *oldpwd = getvar(L VAR_PWD);
    if (oldpwd == NULL) {
	xerror(0, Ngt("$PWD is not set"));
	return Exit_FAILURE;
    }

    bool printnewdir = false;
    size_t stackindex;
    switch (argc - xoptind) {
	case 1:
	    if (wcscmp(ARGV(xoptind), L"-") == 0) {
		newpwd = getvar(L VAR_OLDPWD);
		if (newpwd == NULL || newpwd[0] == L'\0') {
		    xerror(0, Ngt("$OLDPWD is not set"));
		    return Exit_FAILURE;
		}
		printnewdir = true;
		stackindex = SIZE_MAX;
		break;
	    } else {
		newpwd = ARGV(xoptind);
	    }
	    /* falls thru! */
	case 0:
	    if (!parse_dirstack_index(newpwd, &stackindex, &newpwd, true))
		return Exit_FAILURE;
	    break;
	default:
	    goto print_usage;
    }
    assert(newpwd != NULL);

    wchar_t *saveoldpwd = xwcsdup(oldpwd);
    int result = change_directory(newpwd, printnewdir, logical);
#ifndef NDEBUG
    newpwd = NULL;  /* newpwd cannot be used anymore. */
#endif
    if (result != Exit_SUCCESS) {
	free(saveoldpwd);
	return result;
    }

    variable_T *var = get_dirstack();
    if (var == NULL) {
	free(saveoldpwd);
	return Exit_FAILURE;
    }

    push_dirstack(var, saveoldpwd);
    if (stackindex != SIZE_MAX)
	remove_dirstack_entry_at(var, stackindex);
    if (remove_dups)
	remove_dirstack_dups(var);
    return Exit_SUCCESS;
}

/* Returns the directory stack.
 * If the stack is not yet created, it is initialized as an empty stack and
 * returned.
 * Fails if a non-array variable or a readonly array already exists, in which
 * case NULL is returned. */
variable_T *get_dirstack(void)
{
    variable_T *var = search_variable(L VAR_DIRSTACK);
    if (var != NULL) {
	if ((var->v_type & VF_MASK) != VF_ARRAY) {
	    xerror(0, Ngt("$DIRSTACK is not an array"));
	    return NULL;
	} else if (var->v_type & VF_READONLY) {
	    xerror(0, Ngt("$%ls is read-only"), L VAR_DIRSTACK);
	    return NULL;
	}
	return var;
    }

    void **ary = xmalloc(1 * sizeof *ary);
    ary[0] = NULL;
    return set_array(L VAR_DIRSTACK, 0, ary, SCOPE_GLOBAL);
}

/* Adds the specified value to the directory stack ($DIRSTACK).
 * `var' must be the return value of a `get_dirstack' call.
 * `value' is (virtually) freed in this function. */
void push_dirstack(variable_T *var, wchar_t *value)
{
    size_t index = var->v_valc++;
    var->v_vals = xrealloc(var->v_vals, (index + 2) * sizeof *var->v_vals);
    var->v_vals[index] = value;
    var->v_vals[index + 1] = NULL;
}

/* Removes the directory stack entry specified by `index'.
 * `var' must be the directory stack array and `index' must be less than
 * `var->v_valc'. */
void remove_dirstack_entry_at(variable_T *var, size_t index)
{
    assert(index < var->v_valc);
    free(var->v_vals[index]);
    memmove(&var->v_vals[index], &var->v_vals[index + 1],
	    (var->v_valc - index) * sizeof *var->v_vals);
    var->v_valc--;
}

/* Removes directory stack entries that are the same as the current working
 * directory ($PWD).
 * `var' must be the return value of a `get_dirstack' call. */
void remove_dirstack_dups(variable_T *var)
{
    const wchar_t *pwd = getvar(L VAR_PWD);
    if (pwd == NULL)
	return;

    for (size_t index = var->v_valc; index-- > 0; )
	if (wcscmp(pwd, var->v_vals[index]) == 0)
	    remove_dirstack_entry_at(var, index);
}

#if YASH_ENABLE_HELP
const char *pushd_help[] = { Ngt(
"pushd - push a directory into the directory stack\n"
), Ngt(
"\tpushd [-L|-P] [dir]\n"
), Ngt(
"The pushd built-in changes the working directory to <dir> and appends it to\n"
"the directory stack. Options that can be used for the cd built-in can also\n"
"be used for the pushd built-in: -L, -P, and --default-directory=...\n"
), Ngt(
"If <dir> is an integer with the plus or minus sign, it is considered a\n"
"specific entry of the stack, which is removed from the stack and appended\n"
"again. An integer with the plus sign specifies the nth newest entry, and\n"
"a one with the minus sign specifies the nth oldest entry.\n"
), Ngt(
"If neither of <dir> and the --default-directory=... option is specified,\n"
"`+1' is assumed for <dir>.\n"
), Ngt(
"If the --remove-duplicates option is specified, entries that are the same as\n"
"the new working directory are removed from the stack.\n"
), NULL };
#endif

/* The "popd" builtin. */
int popd_builtin(int argc, void **argv)
{
    wchar_t opt;
    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv, L"-", help_option, NULL))) {
	switch (opt) {
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:  print_usage:
		fprintf(stderr, gt("Usage:  popd [index]\n"));
		return Exit_ERROR;
	}
    }

    variable_T *var = get_dirstack();
    if (var == NULL)
	return Exit_FAILURE;
    if (var->v_valc == 0) {
	xerror(0, Ngt("the directory stack is empty"));
	return Exit_FAILURE;
    }

    const wchar_t *arg;
    switch (argc - xoptind) {
	case 0:   arg = L"+0";          break;
	case 1:   arg = ARGV(xoptind);  break;
	default:  goto print_usage;
    }

    size_t stackindex;
    const wchar_t *dummy;
    if (!parse_dirstack_index(arg, &stackindex, &dummy, true))
	return Exit_FAILURE;
    if (stackindex == SIZE_MAX) {
	xerror(0, Ngt("`%ls' is not a valid index"), arg);
	return Exit_ERROR;
    }

    if (stackindex < var->v_valc) {
	remove_dirstack_entry_at(var, stackindex);
	return Exit_SUCCESS;
    }

    int result;
    wchar_t *newpwd;

    assert(var->v_valc > 0);
    var->v_valc--;
    newpwd = var->v_vals[var->v_valc];
    var->v_vals[var->v_valc] = NULL;
    result = change_directory(newpwd, true, true);
    free(newpwd);
    return result;
}

#if YASH_ENABLE_HELP
const char *popd_help[] = { Ngt(
"popd - pop a directory from the directory stack\n"
), Ngt(
"\tpopd [index]\n"
), Ngt(
"The popd built-in removes the last entry from the directory stack, returning\n"
"to the previous directory.\n"
), Ngt(
"If <index> is given, the entry specified by <index> is removed instead of\n"
"the last one. An integer with the plus sign specifies the nth newest entry\n"
"and a one with the minus sign specifies the nth oldest entry.\n"
), NULL };
#endif

/* The "dirs" builtin, which accepts the following options:
 *  -c: clear the stack
 *  -v: verbose */
int dirs_builtin(int argc, void **argv)
{
    static const struct xoption long_options[] = {
	{ L"clear",   OPTARG_NONE, L'c', },
	{ L"verbose", OPTARG_NONE, L'v', },
#if YASH_ENABLE_HELP
	{ L"help",    OPTARG_NONE, L'-', },
#endif
	{ NULL, 0, 0, },
    };

    bool clear = false, verbose = false;

    wchar_t opt;
    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv, L"-cv", long_options, NULL))) {
	switch (opt) {
	    case L'c':  clear   = true;  break;
	    case L'v':  verbose = true;  break;
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:
		fprintf(stderr, gt("Usage:  dirs [-cv] [index...]\n"));
		return Exit_ERROR;
	}
    }

    if (clear)
	return unset_variable(L VAR_DIRSTACK) ? Exit_FAILURE : Exit_SUCCESS;

    variable_T *var = search_variable(L VAR_DIRSTACK);
    bool dirvalid = (var != NULL && (var->v_type & VF_MASK) == VF_ARRAY);
    size_t size = dirvalid ? var->v_valc : 0;
    const wchar_t *dir;
    if (xoptind < argc) {
	/* print the specified only */
	do {
	    size_t index;
	    if (!parse_dirstack_index(ARGV(xoptind), &index, &dir, true))
		continue;
	    if (index == SIZE_MAX) {
		xerror(0, Ngt("`%ls' is not a valid index"), ARGV(xoptind));
		continue;
	    }

	    int r;
	    if (verbose)
		r = printf("+%zu\t-%zu\t%ls\n", size - index, index, dir);
	    else
		r = printf("%ls\n", dir);
	    if (r < 0) {
		xerror(errno, Ngt("cannot print to standard output"));
		break;
	    }
	} while (++xoptind < argc);
    } else {
	/* print all */
	dir = getvar(L VAR_PWD);
	if (dir == NULL) {
	    xerror(0, Ngt("$PWD is not set"));
	} else {
	    int r;
	    if (verbose)
		r = printf("+%zu\t-%zu\t%ls\n", (size_t) 0, size, dir);
	    else
		r = printf("%ls\n", dir);
	    if (r < 0)
		xerror(errno, Ngt("cannot print to standard output"));
	}

	if (dirvalid && yash_error_message_count == 0) {
	    for (size_t i = var->v_valc; i-- > 0; ) {
		int r;
		dir = var->v_vals[i];
		if (verbose)
		    r = printf("+%zu\t-%zu\t%ls\n", size - i, i, dir);
		else
		    r = printf("%ls\n", dir);
		if (r < 0) {
		    xerror(errno, Ngt("cannot print to standard output"));
		    break;
		}
	    }
	}
    }

    return (yash_error_message_count == 0) ? Exit_SUCCESS : Exit_FAILURE;
}

#if YASH_ENABLE_HELP
const char *dirs_help[] = { Ngt(
"dirs - print the directory stack\n"
), Ngt(
"\tdirs [-cv] [index...]\n"
), Ngt(
"With no arguments, the dirs built-in prints the entries of the directory\n"
"stack. If <index> is specified, only the specified entry is printed.\n"
), Ngt(
"The -v (--verbose) option prints the index before each entry.\n"
), Ngt(
"The -c (--clear) option clears the stack.\n"
), NULL };
#endif

#endif /* YASH_ENABLE_DIRSTACK */


/* vim: set ts=8 sts=4 sw=4 noet tw=80: */
