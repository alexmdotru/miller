#include "lib/mlr_globals.h"
#include "lib/mlrutil.h"
#include "lib/string_builder.h"
#include "containers/lrec.h"
#include "containers/sllv.h"
#include "mapping/context_flags.h"
#include "mapping/mlr_dsl_cst.h"
#include "mapping/rval_evaluators.h"
#include "mapping/function_manager.h"
#include "mapping/mappers.h"
#include "dsls/mlr_dsl_wrapper.h"
#include "cli/argparse.h"

typedef struct _mapper_filter_state_t {
	ap_state_t* pargp;
	char* mlr_dsl_expression;
	char* comment_stripped_mlr_dsl_expression;
	mlr_dsl_ast_node_t* past;
	fmgr_t* pfmgr;
	mlhmmv_t* poosvars;
	rval_evaluator_t* pevaluator;
	int do_exclude;
} mapper_filter_state_t;

static void      mapper_filter_usage(FILE* o, char* argv0, char* verb);
static mapper_t* mapper_filter_parse_cli(int* pargi, int argc, char** argv,
	cli_reader_opts_t* _, cli_writer_opts_t* __);
static mapper_t* mapper_filter_alloc(ap_state_t* pargp, char* mlr_dsl_expression, char* comment_stripped_mlr_dsl_expression,
	mlr_dsl_ast_node_t* past, int type_inferencing, int do_exclude);
static void      mapper_filter_free(mapper_t* pmapper);
static sllv_t*   mapper_filter_process(lrec_t* pinrec, context_t* pctx, void* pvstate);

// ----------------------------------------------------------------
mapper_setup_t mapper_filter_setup = {
	.verb = "filter",
	.pusage_func = mapper_filter_usage,
	.pparse_func = mapper_filter_parse_cli,
	.ignores_input = FALSE,
};

// ----------------------------------------------------------------
// xxx -e @ mld/mlh
static void mapper_filter_usage(FILE* o, char* argv0, char* verb) {
	fprintf(o, "Usage: %s %s [options] {expression}\n", argv0, verb);
	fprintf(o, "Prints records for which {expression} evaluates to true.\n");
	fprintf(o, "\n");
	fprintf(o, "Options:\n");
	fprintf(o, "-v: First prints the AST (abstract syntax tree) for the expression, which gives\n");
	fprintf(o, "    full transparency on the precedence and associativity rules of Miller's\n");
	fprintf(o, "    grammar.\n");
	fprintf(o, "-t: Print low-level parser-trace to stderr.\n");
	fprintf(o, "-S: Keeps field values, or literals in the expression, as strings with no type \n");
	fprintf(o, "    inference to int or float.\n");
	fprintf(o, "-F: Keeps field values, or literals in the expression, as strings or floats\n");
	fprintf(o, "    with no inference to int.\n");
	fprintf(o, "-x: Prints records for which {expression} evaluates to false.\n");
	fprintf(o, "-f {filename}: the DSL expression is taken from the specified file rather\n");
	fprintf(o, "    than from the command line. Outer single quotes wrapping the expression\n");
	fprintf(o, "    should not be placed in the file. If -f is specified more than once,\n");
	fprintf(o, "    all input files specified using -f are concatenated to produce the expression.\n");
	fprintf(o, "-e {expression}: You can use this after -f to add an expression. Example use\n");
	fprintf(o, "    case: define functions/subroutines in a file you specify with -f, then call\n");
	fprintf(o, "    them with an expression you specify with -e.\n");
	fprintf(o, "\n");
	fprintf(o, "Please use a dollar sign for field names and double-quotes for string\n");
	fprintf(o, "literals. If field names have special characters such as \".\" then you might\n");
	fprintf(o, "use braces, e.g. '${field.name}'. Miller built-in variables are\n");
	fprintf(o, "NF NR FNR FILENUM FILENAME PI E, and ENV[\"namegoeshere\"] to access environment\n");
	fprintf(o, "variables. The environment-variable name may be an expression, e.g. a field value.\n");
	fprintf(o, "\n");
	fprintf(o, "Use # to comment to end of line.\n");
	fprintf(o, "\n");
	fprintf(o, "Examples:\n");
	fprintf(o, "  %s %s 'log10($count) > 4.0'\n", argv0, verb);
	fprintf(o, "  %s %s 'FNR == 2          (second record in each file)'\n", argv0, verb);
	fprintf(o, "  %s %s 'urand() < 0.001'  (subsampling)\n", argv0, verb);
	fprintf(o, "  %s %s '$color != \"blue\" && $value > 4.2'\n", argv0, verb);
	fprintf(o, "  %s %s '($x<.5 && $y<.5) || ($x>.5 && $y>.5)'\n", argv0, verb);
	fprintf(o, "  %s %s '($name =~ \"^sys.*east$\") || ($name =~ \"^dev.[0-9]+\"i)'\n", argv0, verb);
	fprintf(o, "  %s %s '\n", argv0, verb);
	fprintf(o, "    NR == 1 ||\n");
	fprintf(o, "   #NR == 2 ||\n");
	fprintf(o, "    NR == 3\n");
	fprintf(o, "  '\n");
	fprintf(o, "\n");
	fprintf(o, "Please see http://johnkerl.org/miller/doc/reference.html for more information\n");
	fprintf(o, "including function list. Or \"%s -f\". Please also also \"%s grep\" which is\n", argv0, argv0);
	fprintf(o, "useful when you don't yet know which field name(s) you're looking for.\n");
}

// ----------------------------------------------------------------
static mapper_t* mapper_filter_parse_cli(int* pargi, int argc, char** argv,
	cli_reader_opts_t* _, cli_writer_opts_t* __)
{
	char* verb                                = argv[(*pargi)++];
	slls_t* expression_filenames              = NULL;
	slls_t* expression_strings                = NULL;
	char* mlr_dsl_expression                  = NULL;
	char* comment_stripped_mlr_dsl_expression = NULL;
	int   print_ast                           = FALSE;
	int   trace_parse                         = FALSE;
	int   type_inferencing                    = TYPE_INFER_STRING_FLOAT_INT;
	int   do_exclude                          = FALSE;

	ap_state_t* pstate = ap_alloc();
	ap_define_string_build_list_flag(pstate, "-f", &expression_filenames);
	ap_define_string_build_list_flag(pstate, "-e", &expression_strings);
	ap_define_true_flag(pstate,              "-v", &print_ast);
	ap_define_true_flag(pstate,              "-t", &trace_parse);
	ap_define_int_value_flag(pstate,         "-S", TYPE_INFER_STRING_ONLY,  &type_inferencing);
	ap_define_int_value_flag(pstate,         "-F", TYPE_INFER_STRING_FLOAT, &type_inferencing);
	ap_define_true_flag(pstate,              "-x", &do_exclude);

	// Pass error_on_unrecognized == FALSE to ap_parse so expressions starting
	// with a minus sign aren't treated as errors. Example: "mlr filter '-$x ==
	// $y'".
	if (!ap_parse_aux(pstate, verb, pargi, argc, argv, FALSE)) {
		mapper_filter_usage(stderr, argv[0], verb);
		return NULL;
	}

	if (expression_filenames == NULL && expression_strings == NULL) {
		if ((argc - *pargi) < 1) {
			mapper_filter_usage(stderr, argv[0], verb);
			return NULL;
		}
		mlr_dsl_expression = mlr_strdup_or_die(argv[(*pargi)++]);
	} else {
		string_builder_t *psb = sb_alloc(1024);

		if (expression_filenames != NULL) {
			for (sllse_t* pe = expression_filenames->phead; pe != NULL; pe = pe->pnext) {
				char* expression_filename = pe->value;
				char* mlr_dsl_expression_piece = read_file_into_memory(expression_filename, NULL);
				sb_append_string(psb, mlr_dsl_expression_piece);
				free(mlr_dsl_expression_piece);
			}
		}

		if (expression_strings != NULL) {
			for (sllse_t* pe = expression_strings->phead; pe != NULL; pe = pe->pnext) {
				char* expression_string = pe->value;
				sb_append_string(psb, expression_string);
			}
		}

		mlr_dsl_expression = sb_finish(psb);
		sb_free(psb);
	}
	comment_stripped_mlr_dsl_expression = alloc_comment_strip(mlr_dsl_expression);

	mlr_dsl_ast_t* past = mlr_dsl_parse(comment_stripped_mlr_dsl_expression, trace_parse);
	if (past == NULL) {
		fprintf(stderr, "%s %s: syntax error on DSL parse of '%s'\n",
			argv[0], verb, comment_stripped_mlr_dsl_expression);
		return NULL;
	}

	if (past->proot == NULL) {
		fprintf(stderr, "%s %s: filter statement must not be empty.\n",
			MLR_GLOBALS.bargv0, verb);
		return NULL;
	}

	// For just dev-testing the parser, you can do
	//   mlr filter -v 'expression goes here' /dev/null
	if (print_ast) {
		mlr_dsl_ast_print(past);
	}

	mlr_dsl_ast_node_t* psubtree = extract_filterable_statement(past, type_inferencing);
	return mapper_filter_alloc(pstate, mlr_dsl_expression, comment_stripped_mlr_dsl_expression,
		psubtree, type_inferencing, do_exclude);
}

// ----------------------------------------------------------------
static mapper_t* mapper_filter_alloc(ap_state_t* pargp,
	char* mlr_dsl_expression, char* comment_stripped_mlr_dsl_expression,
	mlr_dsl_ast_node_t* past, int type_inferencing, int do_exclude)
{
	mapper_filter_state_t* pstate = mlr_malloc_or_die(sizeof(mapper_filter_state_t));
	int context_flags = IN_MLR_FILTER;

	pstate->pargp      = pargp;
	// Retain the string contents along with any in-pointers from the AST/CST
	pstate->mlr_dsl_expression = mlr_dsl_expression;
	pstate->comment_stripped_mlr_dsl_expression = comment_stripped_mlr_dsl_expression;
	pstate->past       = past;
	pstate->pfmgr      = fmgr_alloc();
	// xxx make a separate entry point ...
	pstate->pevaluator = rval_evaluator_alloc_from_ast(past, pstate->pfmgr, type_inferencing, context_flags);
	fmgr_resolve_func_callsites(pstate->pfmgr);
	pstate->poosvars   = mlhmmv_alloc();
	pstate->do_exclude = do_exclude;

	mapper_t* pmapper = mlr_malloc_or_die(sizeof(mapper_t));

	pmapper->pvstate       = (void*)pstate;
	pmapper->pprocess_func = mapper_filter_process;
	pmapper->pfree_func    = mapper_filter_free;

	return pmapper;
}

static void mapper_filter_free(mapper_t* pmapper) {
	mapper_filter_state_t* pstate = pmapper->pvstate;
	pstate->pevaluator->pfree_func(pstate->pevaluator);
	ap_free(pstate->pargp);
	mlr_dsl_ast_node_free(pstate->past);
	fmgr_free(pstate->pfmgr);
	mlhmmv_free(pstate->poosvars);
	free(pstate->mlr_dsl_expression);
	free(pstate->comment_stripped_mlr_dsl_expression);
	free(pstate);
	free(pmapper);
}

// ----------------------------------------------------------------
static sllv_t* mapper_filter_process(lrec_t* pinrec, context_t* pctx, void* pvstate) {
	if (pinrec == NULL) // End of input stream
		return sllv_single(NULL);

	mapper_filter_state_t* pstate = pvstate;
	lhmsmv_t* ptyped_overlay = lhmsmv_alloc();
	sllv_t* rv = NULL;

	variables_t variables = (variables_t) {
		.pinrec                   = pinrec,
		.ptyped_overlay           = ptyped_overlay,
		.poosvars                 = pstate->poosvars,
		.ppregex_captures         = NULL,
		.pctx                     = pctx,
		.pbind_stack              = NULL,
		.ploop_stack              = NULL,
		.return_state = {
			.returned = FALSE,
			.retval = mv_absent(),
		}
	};

	mv_t val = pstate->pevaluator->pprocess_func(pstate->pevaluator->pvstate, &variables);

	if (mv_is_null(&val)) {
		lrec_free(pinrec);
	} else {
		mv_set_boolean_strict(&val);
		if (val.u.boolv ^ pstate->do_exclude) {
			rv = sllv_single(pinrec);
		} else {
			lrec_free(pinrec);
		}
	}

	lhmsmv_free(ptyped_overlay);
	return rv;
}
