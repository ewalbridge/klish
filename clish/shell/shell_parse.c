/*
 * shell_parse.c
 */

#include <string.h>
#include <assert.h>

#include "lub/string.h"
#include "lub/system.h"
#include "private.h"


/*----------------------------------------------------------- */
/* The standard clish_param_validate() is not enough when PTYPE can
 * contain ACTION. So we need context etc. to really validate param.
 */
static char *clish_shell_param_validate(const clish_param_t *param, const char *text,
	clish_context_t *context)
{
	clish_ptype_t *ptype = NULL;
	clish_ptype_method_e method = CLISH_PTYPE_METHOD_MAX;
	char *out = NULL;
	clish_context_t ctx = {};
	clish_pargv_t *pargv = NULL;
	clish_param_t *value_param = NULL;
	int retval = 0;

	assert(param);
	assert(context);
	if (!param || !context)
		return NULL;

	ptype = clish_param__get_ptype(param);
	assert(ptype);
	if (!ptype)
		return NULL;
	method = clish_ptype__get_method(ptype);

	// Check is it common non-code PTYPE
	if (method != CLISH_PTYPE_METHOD_CODE)
		return clish_param_validate(param, text);

	// Prepare dummy pargv structure to provide 'value' parameter to the
	// ACTION script. This parameter contain current value to check.
	value_param = clish_param_new("value", "Dummy param for PTYPE's ACTION",
		clish_param__get_ptype_name(param));
	assert(value_param);
	if (!value_param)
		return NULL;
	clish_param__set_ptype(value_param, ptype);
	pargv = clish_pargv_new(); // Dummy pargv
	assert(pargv);
	if (!pargv)
		return NULL;
	clish_pargv_insert(pargv, value_param, text);

	// Prepare context for ACTION execution
	clish_context_dup(&ctx, context);
	clish_context__set_action(&ctx, clish_ptype__get_action(ptype));
	clish_context__set_pargv(&ctx, pargv);

	// Try to execute ACTION
	retval = clish_shell_exec_action(&ctx, &out);
	// Cleanup dummy structures
	clish_pargv_delete(pargv);
	clish_param_delete(value_param);

	if (retval) {
		lub_string_free(out);
		return NULL; // Fail on bad ACTION retval
	}
	if (out) {
		if (*out != '\0') // Non-empty str is transformation
			return out;
		lub_string_free(out);
	}

	return lub_string_dup(text);
}

/*----------------------------------------------------------- */
clish_pargv_status_e clish_shell_parse(
	clish_shell_t *this, const char *line,
	const clish_command_t **ret_cmd, clish_pargv_t **pargv)
{
	clish_pargv_status_e result = CLISH_BAD_CMD;
	clish_context_t context;
	const clish_command_t *cmd;
	lub_argv_t *argv = NULL;
	unsigned int idx;

	*ret_cmd = cmd = clish_shell_resolve_command(this, line);
	if (!cmd)
		return result;

	/* Now construct the parameters for the command */
	/* Prepare context */
	*pargv = clish_pargv_new();
	clish_context_init(&context, this);
	clish_context__set_cmd(&context, cmd);
	clish_context__set_pargv(&context, *pargv);

	idx = lub_string_wordcount(clish_command__get_name(cmd));
	argv = lub_argv_new(line, 0);
	result = clish_shell_parse_pargv(*pargv, cmd, &context,
		clish_command__get_paramv(cmd),
		argv, &idx, NULL, 0);
	lub_argv_delete(argv);
	if (CLISH_LINE_OK != result) {
		clish_pargv_delete(*pargv);
		*pargv = NULL;
	}

	return result;
}

/*--------------------------------------------------------- */
static bool_t line_test(const clish_param_t *param, void *context)
{
	char *str = NULL;
	const char *teststr = NULL;
	bool_t res;

	if (!param)
		return BOOL_FALSE;
	teststr = clish_param__get_test(param);
	if (!teststr)
		return BOOL_TRUE;
	str = clish_shell_expand(teststr, SHELL_VAR_ACTION, context);
	if (!str)
		return BOOL_FALSE;
	res = lub_system_line_test(str);
	lub_string_free(str);

	return res;
}

/*--------------------------------------------------------- */
clish_pargv_status_e clish_shell_parse_pargv(clish_pargv_t *pargv,
	const clish_command_t *cmd,
	void *context,
	clish_paramv_t *paramv,
	const lub_argv_t *argv,
	unsigned *idx, clish_pargv_t *last, unsigned need_index)
{
	unsigned argc = lub_argv__get_count(argv);
	unsigned index = 0;
	unsigned nopt_index = 0;
	clish_param_t *nopt_param = NULL;
	unsigned i;
	clish_pargv_status_e retval;
	unsigned paramc = clish_paramv__get_count(paramv);
	int up_level = 0; /* Is it a first level of param nesting? */

	assert(pargv);
	assert(cmd);

	/* Check is it a first level of PARAM nesting. */
	if (paramv == clish_command__get_paramv(cmd))
		up_level = 1;

	while (index < paramc) {
		const char *arg = NULL;
		clish_param_t *param = clish_paramv__get_param(paramv, index);
		clish_param_t *cparam = NULL;
		int is_switch = 0;

		if (!param)
			return CLISH_BAD_PARAM;

		/* Use real arg or PARAM's default value as argument */
		if (*idx < argc)
			arg = lub_argv__get_arg(argv, *idx);

		/* Is parameter in "switch" mode? */
		if (CLISH_PARAM_SWITCH == clish_param__get_mode(param))
			is_switch = 1;

		/* Check the 'test' conditions */
		if (!line_test(param, context)) {
			index++;
			continue;
		}

		/* Add param for help and completion */
		if (last && (*idx == need_index) &&
			(NULL == clish_pargv_find_arg(pargv, clish_param__get_name(param)))) {
			if (is_switch) {
				unsigned rec_paramc = clish_param__get_param_count(param);
				for (i = 0; i < rec_paramc; i++) {
					cparam = clish_param__get_param(param, i);
					if (!cparam)
						break;
					if (!line_test(cparam, context))
						continue;
					if (CLISH_PARAM_SUBCOMMAND ==
						clish_param__get_mode(cparam)) {
						const char *pname =
							clish_param__get_value(cparam);
						if (!arg || (arg && 
							(pname == lub_string_nocasestr(pname,
							arg))))
							clish_pargv_insert(last,
								cparam, arg);
					} else {
						clish_pargv_insert(last,
							cparam, arg);
					}
				}
			} else {
				if (CLISH_PARAM_SUBCOMMAND ==
					clish_param__get_mode(param)) {
					const char *pname =
					    clish_param__get_value(param);
					if (!arg || (arg &&
						(pname == lub_string_nocasestr(pname, arg))))
						clish_pargv_insert(last, param, arg);
				} else {
					clish_pargv_insert(last, param, arg);
				}
			}
		}

		/* Set parameter value */
		{
			char *validated = NULL;
			clish_paramv_t *rec_paramv =
			    clish_param__get_paramv(param);
			unsigned rec_paramc =
			    clish_param__get_param_count(param);

			/* Save the index of last non-option parameter
			 * to restore index if the optional parameters
			 * will be used.
			 */
			if (!clish_param__get_optional(param)) {
				nopt_param = param;
				nopt_index = index;
			}

			/* Validate the current parameter. */
			if (clish_pargv_find_arg(pargv, clish_param__get_name(param))) {
				/* Duplicated parameter */
				validated = NULL;
			} else if (is_switch) {
				for (i = 0; i < rec_paramc; i++) {
					cparam = clish_param__get_param(param, i);
					if (!cparam)
						break;
					if (!line_test(cparam, context))
						continue;
					if ((validated = arg ?
						clish_shell_param_validate(
							cparam, arg, context) :
							NULL)) {
						rec_paramv = clish_param__get_paramv(cparam);
						rec_paramc = clish_param__get_param_count(cparam);
						break;
					}
				}
			} else {
				validated = arg ?
					clish_shell_param_validate(
						param, arg, context) : NULL;
			}

			if (validated) {
				/* add (or update) this parameter */
				if (is_switch) {
					clish_pargv_insert(pargv, param,
						clish_param__get_name(cparam));
					clish_pargv_insert(pargv, cparam,
						validated);
				} else {
					clish_pargv_insert(pargv, param,
						validated);
				}
				lub_string_free(validated);

				/* Next command line argument */
				/* Don't change idx if this is the last
				   unfinished optional argument.
				 */
				if (!(clish_param__get_optional(param) &&
					(*idx == need_index) &&
					(need_index == (argc - 1)))) {
					(*idx)++;
					/* Walk through the nested parameters */
					if (rec_paramc) {
						retval = clish_shell_parse_pargv(pargv, cmd,
							context, rec_paramv,
							argv, idx, last, need_index);
						if (CLISH_LINE_OK != retval)
							return retval;
					}
				}

				/* Choose the next parameter */
				if (clish_param__get_optional(param) &&
					!clish_param__get_order(param)) {
					if (nopt_param)
						index = nopt_index + 1;
					else
						index = 0;
				} else {
					/* Save non-option position in
					   case of ordered optional param */
					nopt_param = param;
					nopt_index = index;
					index++;
				}

			} else {
				/* Choose the next parameter if current
				 * is not validated.
				 */
				if (clish_param__get_optional(param))
					index++;
				else {
					if (!arg)
						break;
					else
						return CLISH_BAD_PARAM;
				}
			}
		}
	}

	/* Check for non-optional parameters without values */
	if ((*idx >= argc) && (index < paramc)) {
		unsigned j = index;
		const clish_param_t *param;
		while (j < paramc) {
			param = clish_paramv__get_param(paramv, j++);
			if (BOOL_TRUE != clish_param__get_optional(param))
				return CLISH_LINE_PARTIAL;
		}
	}

	/* If the number of arguments is bigger than number of
	 * params than it's a args. So generate the args entry
	 * in the list of completions.
	 */
	if (last && up_level &&
			clish_command__get_args(cmd) &&
			(clish_pargv__get_count(last) == 0) &&
			(*idx <= argc) && (index >= paramc)) {
		clish_pargv_insert(last, clish_command__get_args(cmd), "");
	}

	/*
	 * if we've satisfied all the parameters we can now construct
	 * an 'args' parameter if one exists
	 */
	if (up_level && (*idx < argc) && (index >= paramc)) {
		const char *arg = lub_argv__get_arg(argv, *idx);
		const clish_param_t *param = clish_command__get_args(cmd);
		char *args = NULL;

		if (!param)
			return CLISH_BAD_CMD;

		/*
		 * put all the argument into a single string
		 */
		while (NULL != arg) {
			bool_t quoted = lub_argv__get_quoted(argv, *idx);
			char *enc = NULL;
			if (BOOL_TRUE == quoted) {
				lub_string_cat(&args, "\"");
			}
			/* place the current argument in the string */
			/* Escape quote and backslash */
			enc = lub_string_encode(arg, lub_string_esc_quoted);
			lub_string_cat(&args, enc);
			lub_string_free(enc);
			if (BOOL_TRUE == quoted) {
				lub_string_cat(&args, "\"");
			}
			(*idx)++;
			arg = lub_argv__get_arg(argv, *idx);
			if (NULL != arg) {
				/* add a space if there are more arguments */
				lub_string_cat(&args, " ");
			}
		}
		/* add (or update) this parameter */
		clish_pargv_insert(pargv, param, args);
		lub_string_free(args);
	}

	return CLISH_LINE_OK;
}

CLISH_SET(shell, clish_shell_state_e, state);
CLISH_GET(shell, clish_shell_state_e, state);
