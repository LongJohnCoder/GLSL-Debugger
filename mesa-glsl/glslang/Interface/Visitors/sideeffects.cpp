/*
 * sideeffects.cpp
 *
 *  Created on: 27.12.2013
 *
 * There a lot of functions as in ast_to_hir.
 * Primary purpose of it to obtain glsl type for ShVariable
 * Error messages was removed because it is driver work to compile
 * and check for errors, i.e. errors in shader -> no shader for debugging.
 * If you have some problems with this behavior, please report it,
 * it probably some errors in shader receiving phase, not here.
 */

#include "sideeffects.h"
#include "glsl/ast.h"
#include "glsl/ir.h"
#include "glsl/list.h"
#include "glslang/Interface/CodeTools.h"
#include "glslang/Include/ShaderLang.h"
#undef NDEBUG
#include <assert.h>


extern void validate_identifier(const char *, YYLTYPE, struct _mesa_glsl_parse_state *);
extern const glsl_type* process_array_type(YYLTYPE*, const glsl_type*, ast_array_specifier*,
		struct _mesa_glsl_parse_state*);
extern glsl_interp_qualifier interpret_interpolation_qualifier(
		const struct ast_type_qualifier *, ir_variable_mode, struct _mesa_glsl_parse_state*,
		YYLTYPE*);
extern void validate_matrix_layout_for_type(struct _mesa_glsl_parse_state *, YYLTYPE *,
		const glsl_type *, ir_variable *);
extern unsigned process_parameters(exec_list *instructions, exec_list *actual_parameters,
		exec_list *parameters, struct _mesa_glsl_parse_state *state);
extern void apply_type_qualifier_to_variable(const struct ast_type_qualifier *qual,
		ir_variable *var, struct _mesa_glsl_parse_state *state, YYLTYPE *loc,
		bool is_parameter);
extern void handle_geometry_shader_input_decl(struct _mesa_glsl_parse_state *state,
		YYLTYPE loc, ir_variable *var);
extern ir_variable * get_variable_being_redeclared(ir_variable *var, YYLTYPE loc,
		struct _mesa_glsl_parse_state *state, bool allow_all_redeclarations);


// As in ast_to_hir
unsigned ast_process_structure_or_interface_block(ast_sideeffects_traverser_visitor* visitor,
		struct _mesa_glsl_parse_state *state, exec_list *declarations, YYLTYPE &loc,
		glsl_struct_field **fields_ret, bool block_row_major)
{
	unsigned decl_count = 0;
	foreach_list_typed (ast_declarator_list, decl_list, link, declarations) {
		foreach_list_const (decl_ptr, & decl_list->declarations) {
			decl_count++;
		}
	}

	glsl_struct_field * const fields = ralloc_array(state, glsl_struct_field,
			decl_count);

	unsigned i = 0;
	foreach_list_typed (ast_declarator_list, decl_list, link, declarations) {
		const char *type_name;
		decl_list->type->specifier->accept(visitor);
		const glsl_type *decl_type = decl_list->type->glsl_type(&type_name, state);
		foreach_list_typed (ast_declaration, decl, link, &decl_list->declarations) {
			validate_identifier(decl->identifier, loc, state);

			const struct glsl_type *field_type =
					decl_type != NULL ? decl_type : glsl_type::error_type;

			const struct ast_type_qualifier * const qual = &decl_list->type->qualifier;
			field_type = process_array_type(&loc, decl_type, decl->array_specifier, state);
			fields[i].type = field_type;
			fields[i].name = decl->identifier;
			fields[i].location = -1;
			fields[i].interpolation = interpret_interpolation_qualifier(qual, ir_var_auto,
					state, &loc);
			fields[i].centroid = qual->flags.q.centroid ? 1 : 0;
			fields[i].sample = qual->flags.q.sample ? 1 : 0;

			if (qual->flags.q.row_major || qual->flags.q.column_major) {
				if (qual->flags.q.uniform)
					validate_matrix_layout_for_type(state, &loc, field_type, NULL);
			}

			if (field_type->is_matrix()
					|| (field_type->is_array() && field_type->fields.array->is_matrix())) {
				fields[i].row_major = block_row_major;
				if (qual->flags.q.row_major)
					fields[i].row_major = true;
				else if (qual->flags.q.column_major)
					fields[i].row_major = false;
			}

			i++;
		}
	}

	assert(i == decl_count);

	*fields_ret = fields;
	return decl_count;
}

void ast_sideeffects_traverser_visitor::visit(ast_declarator_list* node)
{
	assert(node->type);
	node->type->accept(this);

	const char* type_name;
	const glsl_type * decl_type = node->type->glsl_type(&type_name, this->state);
	assert(decl_type || !"Using of non-declared type");

	foreach_list_typed (ast_declaration, decl, link, &node->declarations) {
		if (decl->initializer && decl->initializer->oper == ast_aggregate)
			_mesa_ast_set_aggregate_type(decl_type, decl->initializer);

		decl->accept(this);
		// Register variable
		variableQualifier qual = qualifierFromAst(&node->type->qualifier, false);
		variableVaryingModifier modifier = modifierFromAst(&node->type->qualifier);
		astToShVariable(decl, qual, modifier, decl_type, shader);

		YYLTYPE loc = decl->get_location();
		const struct glsl_type *var_type;
		ir_variable *var;

		if ((decl_type == NULL) || decl_type->is_void())
			continue;

		var_type = process_array_type(&loc, decl_type, decl->array_specifier, state);
		var = new(shader) ir_variable(var_type, decl->identifier, ir_var_auto);
		apply_type_qualifier_to_variable(&node->type->qualifier, var, state, &loc, false);
		if (var->data.mode == ir_var_shader_in) {
			var->data.read_only = true;
			if (state->stage == MESA_SHADER_GEOMETRY)
				handle_geometry_shader_input_decl(state, loc, var);
			input_variables.push_tail(var);
		}

		if (!get_variable_being_redeclared(var, loc, state, false)) {
			validate_identifier(decl->identifier, loc, state);
			if (!state->symbols->add_variable(var))
				_mesa_glsl_error(&loc, state, "name `%s' already taken in the "
						"current scope", decl->identifier);
		}
	}
}

// As in ast_to_hir
void ast_sideeffects_traverser_visitor::visit(ast_struct_specifier* node)
{
	YYLTYPE loc = node->get_location();
	if (state->language_version != 110 && state->struct_specifier_depth != 0)
		_mesa_glsl_error(&loc, state, "embedded structure declartions are not allowed");

	state->struct_specifier_depth++;

	glsl_struct_field *fields;
	unsigned decl_count = ast_process_structure_or_interface_block(this, state,
			&node->declarations, loc, &fields, false);

	validate_identifier(node->name, loc, state);
	const glsl_type *t = glsl_type::get_record_instance(fields, decl_count, node->name);
	foreach_list_typed (ast_declarator_list, decl_list, link, &node->declarations) {
		const struct ast_type_qualifier * const qual = &decl_list->type->qualifier;
		variableQualifier qualifier = qualifierFromAst(qual, false);
		variableVaryingModifier modifier = modifierFromAst(qual);
		ShVariable* var = astToShVariable(node, qualifier, modifier, t, shader);
		int i = 0;
		// assume we have same count of fields and declarations
		foreach_list_typed (ast_declaration, decl, link, &decl_list->declarations) {
			ShVariable* field = var->structSpec[i++];
			addAstShVariable(decl, field);
		}
	}

	if (!state->symbols->add_type(node->name, t)) {
		_mesa_glsl_error(&loc, state, "struct `%s' previously defined", node->name);
	} else {
		const glsl_type **s = reralloc(state, state->user_structures,
				const glsl_type *, state->num_user_structures + 1);
		if (s != NULL) {
			s[state->num_user_structures] = t;
			state->user_structures = s;
			state->num_user_structures++;
		}
	}

	state->struct_specifier_depth--;
}

// As in ast_to_hir
void ast_sideeffects_traverser_visitor::visit(ast_parameter_declarator *node)
{
	const struct glsl_type *type;
	const char *name = NULL;
	YYLTYPE loc = node->get_location();

	type = node->type->glsl_type(&name, state);

	if (type == NULL)
		type = glsl_type::error_type;

	if (node->formal_parameter && (node->identifier == NULL))
		return;

	node->is_void = false;
	if (type->is_void()) {
		node->is_void = true;
		return;
	}

	type = process_array_type(&loc, type, node->array_specifier, state);
	if (!type->is_error() && type->is_unsized_array())
		type = glsl_type::error_type;

	if (type == glsl_type::error_type)
		_mesa_glsl_error(&loc, state, "Error while processing parameter.");

	variableQualifier qual = qualifierFromAst(&node->type->qualifier, true);
	variableVaryingModifier modifier = modifierFromAst(&node->type->qualifier);
	astToShVariable(node, qual, modifier, type, shader);

	if (!type->is_error() && type->is_unsized_array()) {
		_mesa_glsl_error(&loc, state, "arrays passed as parameters must have "
				"a declared size");
		type = glsl_type::error_type;
	}

	ir_variable *var = new(shader) ir_variable(type, node->identifier, ir_var_function_in);
	apply_type_qualifier_to_variable(&node->type->qualifier, var, state, &loc, true);
	if (!get_variable_being_redeclared(var, loc, state, false)) {
		validate_identifier(node->identifier, loc, state);
		if (!state->symbols->add_variable(var))
			_mesa_glsl_error(&loc, state, "name `%s' already taken in the "
							"current scope", node->identifier);
	}
}

void ast_sideeffects_traverser_visitor::visit(ast_compound_statement* node)
{
	if (node->new_scope){
		state->symbols->push_scope();
		depth++;
	}

	foreach_list_typed (ast_node, ast, link, &node->statements) {
		ast->accept(this);
		node->debug_sideeffects |= ast->debug_sideeffects;
	}

	if (node->new_scope){
		state->symbols->pop_scope();
		depth--;
	}
}

void ast_sideeffects_traverser_visitor::visit(ast_expression_statement* node)
{
	if (node->expression) {
		node->expression->accept(this);
		node->debug_sideeffects |= node->expression->debug_sideeffects;
	}
}

void ast_sideeffects_traverser_visitor::visit(ast_function_definition * node)
{
	state->symbols->push_scope();
	ast_traverse_visitor::visit(node);
	state->symbols->pop_scope();
}

void ast_sideeffects_traverser_visitor::visit(ast_selection_statement *node)
{
	if (flags & traverse_previsit)
		if (!this->traverse(node))
			return;

	if (node->condition)
		node->condition->accept(this);
	++depth;
	if (node->then_statement){
		state->symbols->push_scope();
		node->then_statement->accept(this);
		state->symbols->pop_scope();
	}
	if (node->else_statement){
		state->symbols->push_scope();
		node->else_statement->accept(this);
		state->symbols->pop_scope();
	}
	--depth;

	if (flags & traverse_postvisit)
		this->traverse(node);
}

void ast_sideeffects_traverser_visitor::visit(ast_iteration_statement *node)
{
	if (flags & traverse_previsit)
		if (!this->traverse(node))
			return;

	if (node->mode != ast_iteration_statement::ast_do_while)
		state->symbols->push_scope();

	if (node->init_statement)
		node->init_statement->accept(this);
	if (node->condition)
		node->condition->accept(this);
	++depth;
	if (node->body)
		node->body->accept(this);
	--depth;
	if (node->rest_expression)
		node->rest_expression->accept(this);

	if (node->mode != ast_iteration_statement::ast_do_while)
			state->symbols->pop_scope();

	if (flags & traverse_postvisit)
		this->traverse(node);
}

bool ast_sideeffects_traverser_visitor::traverse(ast_expression* node)
{
	for (int i = 0; i < 3; ++i) {
		if (!node->subexpressions[i])
			continue;
		node->debug_sideeffects |= node->subexpressions[i]->debug_sideeffects;
	}

	switch (node->oper) {
	case ast_assign:
	case ast_mul_assign:
	case ast_div_assign:
	case ast_mod_assign:
	case ast_add_assign:
	case ast_sub_assign:
	case ast_ls_assign:
	case ast_rs_assign:
	case ast_and_assign:
	case ast_xor_assign:
	case ast_or_assign:
	case ast_pre_inc:
	case ast_pre_dec:
	case ast_post_inc:
	case ast_post_dec:
		node->debug_sideeffects |= ast_dbg_se_general;
		break;

	case ast_sequence:
	case ast_function_call:
	case ast_aggregate:
		assert(!"not implemented");
		break;

	case ast_identifier:
		// It may be deref or global variable
		if (node->debug_id < 0){
			ir_variable* var = state->symbols->get_variable(
					node->primary_expression.identifier);
			assert(var || !"Undeclared identifier");
			const glsl_type* type = var->type;
			variableQualifier qual = qualifierFromIr(var);
			variableVaryingModifier modifier = modifierFromIr(var);
			astToShVariable(node, qual, modifier, type, shader);
		}
		break;
	case ast_field_selection: {
		// We need to know is it struct or swizzle
		// In mesa it obtained in some wierd way.
		exec_list instructions;
		ir_rvalue* op = node->subexpressions[0]->hir(&instructions, state);
		if (op->type->base_type == GLSL_TYPE_STRUCT
		              || op->type->base_type == GLSL_TYPE_INTERFACE)
			node->debug_selection_type = ast_fst_struct;
		else if (node->subexpressions[1] != NULL)
			node->debug_selection_type = ast_fst_method;
		else if (op->type->is_vector() ||
	              (state->ARB_shading_language_420pack_enable &&
	               op->type->is_scalar()))
			node->debug_selection_type = ast_fst_swizzle;
		else
			assert(!"wrong type for field selection");
		break;
	}
	default:
		break;
	}

	return true;
}

bool ast_sideeffects_traverser_visitor::traverse(ast_expression_bin* node)
{
	node->debug_sideeffects |= node->subexpressions[0]->debug_sideeffects;
	node->debug_sideeffects |= node->subexpressions[1]->debug_sideeffects;
	return true;
}

bool ast_sideeffects_traverser_visitor::traverse(ast_function_expression* node)
{
	// Not sure about it
	foreach_list_const(n, &node->expressions) {
		ast_node *ast = exec_node_data(ast_node, n, link);
		node->debug_sideeffects |= ast->debug_sideeffects;
	}

	node->debug_builtin = false;
	if (node->is_constructor()) {
		node->debug_builtin = true;
	} else {
		ast_expression* id = node->subexpressions[0];
		const char *func_name = id->primary_expression.identifier;
		if (!strcmp(func_name, EMIT_VERTEX_SIGNATURE))
			node->debug_sideeffects |= ast_dbg_se_emit_vertex;

		// Probably leak
		exec_list actual_parameters;
		exec_list instructions;
		process_parameters(&instructions, &actual_parameters, &node->expressions, state);

		/* Local shader has no exact candidates; check the built-ins. */
		_mesa_glsl_initialize_builtin_functions();
		if (_mesa_glsl_find_builtin_function(state, func_name, &actual_parameters))
			node->debug_builtin = true;
	}

	return true;
}

bool ast_sideeffects_traverser_visitor::traverse(ast_case_statement* node)
{
	foreach_list_typed (ast_node, ast, link, &node->stmts)
		node->debug_sideeffects |= ast->debug_sideeffects;
	return true;
}

bool ast_sideeffects_traverser_visitor::traverse(ast_switch_body* node)
{
	if (node->stmts)
		foreach_list_typed (ast_node, ast, link, &node->stmts->cases)
			node->debug_sideeffects |= ast->debug_sideeffects;
	return true;
}

bool ast_sideeffects_traverser_visitor::traverse(ast_selection_statement* node)
{
	if (node->condition)
		node->debug_sideeffects |= node->condition->debug_sideeffects;
	if (node->then_statement)
		node->debug_sideeffects |= node->then_statement->debug_sideeffects;
	if (node->else_statement)
		node->debug_sideeffects |= node->else_statement->debug_sideeffects;
	return true;
}

bool ast_sideeffects_traverser_visitor::traverse(ast_switch_statement* node)
{
	if (node->test_expression)
		node->debug_sideeffects |= node->test_expression->debug_sideeffects;
	if (node->body)
		node->debug_sideeffects |= node->body->debug_sideeffects;
	return true;
}

bool ast_sideeffects_traverser_visitor::traverse(ast_iteration_statement* node)
{
	if (node->init_statement)
		node->debug_sideeffects |= node->init_statement->debug_sideeffects;
	if (node->condition)
		node->debug_sideeffects |= node->condition->debug_sideeffects;
	if (node->body)
		node->debug_sideeffects |= node->body->debug_sideeffects;
	if (node->rest_expression)
		node->debug_sideeffects |= node->rest_expression->debug_sideeffects;
	return true;
}

bool ast_sideeffects_traverser_visitor::traverse(ast_jump_statement* node)
{
	if (node->mode == ast_jump_statement::ast_discard)
		node->debug_sideeffects |= ir_dbg_se_discard;
	return true;
}

bool ast_sideeffects_traverser_visitor::traverse(ast_function_definition* node)
{
	node->debug_sideeffects |= ir_dbg_se_general;
	saveFunction(node->prototype);
	return true;
}

bool ast_sideeffects_traverser_visitor::traverse(ast_gs_input_layout* node)
{
	YYLTYPE loc = node->get_location();
	state->gs_input_prim_type_specified = true;
	state->gs_input_prim_type = node->prim_type;
	unsigned num_vertices = vertices_per_prim(node->prim_type);

	/* If any shader inputs occurred before this declaration and did not
	 * specify an array size, their size is determined now. */
	foreach_list (node, &input_variables) {
		ir_variable *var = ((ir_instruction *) node)->as_variable();
		if (var == NULL || var->data.mode != ir_var_shader_in)
			continue;

		if (var->type->is_unsized_array()) {
			if (var->data.max_array_access >= num_vertices)
				_mesa_glsl_error(&loc, state, "this geometry shader input layout implies %u"
						" vertices, but an access to element %u of input"
						" `%s' already exists", num_vertices, var->data.max_array_access,
						var->name);
			else
				var->type = glsl_type::get_array_instance(var->type->fields.array,
						num_vertices);
		}
	}
	return true;
}

