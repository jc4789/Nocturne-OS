/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output, and Bison version.  */
#define YYBISON 30802

/* Bison version string.  */
#define YYBISON_VERSION "3.8.2"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 1

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1


/* Substitute the variable and function names.  */
#define yyparse         _mesa_glsl_parse
#define yylex           _mesa_glsl_lex
#define yyerror         _mesa_glsl_error
#define yydebug         _mesa_glsl_debug
#define yynerrs         _mesa_glsl_nerrs

/* First part of user prologue.  */
#line 1 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"

/*
 * Copyright © 2008, 2009 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Nocturne declares strcasecmp in <string.h>. */
#if !defined(_MSC_VER) && !defined(NOCTURNE)
#include <strings.h>
#endif
#include <assert.h>

#include "ast.h"
#include "glsl_parser_extras.h"
#include "compiler/glsl_types.h"
#include "util/blend.h"
#include "util/u_string.h"
#include "util/format/u_format.h"
#include "main/consts_exts.h"

#ifdef _MSC_VER
#pragma warning( disable : 4065 ) // switch statement contains 'default' but no 'case' labels
#endif

#undef yyerror

static void yyerror(YYLTYPE *loc, _mesa_glsl_parse_state *st, const char *msg)
{
   _mesa_glsl_error(loc, st, "%s", msg);
}

static int
_mesa_glsl_lex(YYSTYPE *val, YYLTYPE *loc, _mesa_glsl_parse_state *state)
{
   return _mesa_glsl_lexer_lex(val, loc, state->scanner);
}

static bool match_layout_qualifier(const char *s1, const char *s2,
                                   _mesa_glsl_parse_state *state)
{
   /* From the GLSL 1.50 spec, section 4.3.8 (Layout Qualifiers):
    *
    *     "The tokens in any layout-qualifier-id-list ... are not case
    *     sensitive, unless explicitly noted otherwise."
    *
    * The text "unless explicitly noted otherwise" appears to be
    * vacuous--no desktop GLSL spec (up through GLSL 4.40) notes
    * otherwise.
    *
    * However, the GLSL ES 3.00 spec says, in section 4.3.8 (Layout
    * Qualifiers):
    *
    *     "As for other identifiers, they are case sensitive."
    *
    * So we need to do a case-sensitive or a case-insensitive match,
    * depending on whether we are compiling for GLSL ES.
    */
   if (state->es_shader)
      return strcmp(s1, s2);
   else
      return strcasecmp(s1, s2);
}

#line 160 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

#include "glsl_parser.h"
/* Symbol kind.  */
enum yysymbol_kind_t
{
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,                      /* "end of file"  */
  YYSYMBOL_YYerror = 1,                    /* error  */
  YYSYMBOL_YYUNDEF = 2,                    /* "invalid token"  */
  YYSYMBOL_ATTRIBUTE = 3,                  /* ATTRIBUTE  */
  YYSYMBOL_CONST_TOK = 4,                  /* CONST_TOK  */
  YYSYMBOL_BASIC_TYPE_TOK = 5,             /* BASIC_TYPE_TOK  */
  YYSYMBOL_BREAK = 6,                      /* BREAK  */
  YYSYMBOL_BUFFER = 7,                     /* BUFFER  */
  YYSYMBOL_CONTINUE = 8,                   /* CONTINUE  */
  YYSYMBOL_DO = 9,                         /* DO  */
  YYSYMBOL_ELSE = 10,                      /* ELSE  */
  YYSYMBOL_FOR = 11,                       /* FOR  */
  YYSYMBOL_IF = 12,                        /* IF  */
  YYSYMBOL_DEMOTE = 13,                    /* DEMOTE  */
  YYSYMBOL_DISCARD = 14,                   /* DISCARD  */
  YYSYMBOL_RETURN = 15,                    /* RETURN  */
  YYSYMBOL_SWITCH = 16,                    /* SWITCH  */
  YYSYMBOL_CASE = 17,                      /* CASE  */
  YYSYMBOL_DEFAULT = 18,                   /* DEFAULT  */
  YYSYMBOL_CENTROID = 19,                  /* CENTROID  */
  YYSYMBOL_IN_TOK = 20,                    /* IN_TOK  */
  YYSYMBOL_OUT_TOK = 21,                   /* OUT_TOK  */
  YYSYMBOL_INOUT_TOK = 22,                 /* INOUT_TOK  */
  YYSYMBOL_UNIFORM = 23,                   /* UNIFORM  */
  YYSYMBOL_VARYING = 24,                   /* VARYING  */
  YYSYMBOL_SAMPLE = 25,                    /* SAMPLE  */
  YYSYMBOL_NOPERSPECTIVE = 26,             /* NOPERSPECTIVE  */
  YYSYMBOL_FLAT = 27,                      /* FLAT  */
  YYSYMBOL_SMOOTH = 28,                    /* SMOOTH  */
  YYSYMBOL_PIXEL_LOCAL = 29,               /* PIXEL_LOCAL  */
  YYSYMBOL_PIXEL_LOCAL_IN = 30,            /* PIXEL_LOCAL_IN  */
  YYSYMBOL_PIXEL_LOCAL_OUT = 31,           /* PIXEL_LOCAL_OUT  */
  YYSYMBOL_IMAGE1DSHADOW = 32,             /* IMAGE1DSHADOW  */
  YYSYMBOL_IMAGE2DSHADOW = 33,             /* IMAGE2DSHADOW  */
  YYSYMBOL_IMAGE1DARRAYSHADOW = 34,        /* IMAGE1DARRAYSHADOW  */
  YYSYMBOL_IMAGE2DARRAYSHADOW = 35,        /* IMAGE2DARRAYSHADOW  */
  YYSYMBOL_COHERENT = 36,                  /* COHERENT  */
  YYSYMBOL_VOLATILE = 37,                  /* VOLATILE  */
  YYSYMBOL_RESTRICT = 38,                  /* RESTRICT  */
  YYSYMBOL_READONLY = 39,                  /* READONLY  */
  YYSYMBOL_WRITEONLY = 40,                 /* WRITEONLY  */
  YYSYMBOL_SHARED = 41,                    /* SHARED  */
  YYSYMBOL_TASKPAYLOAD = 42,               /* TASKPAYLOAD  */
  YYSYMBOL_PERPRIMITIVE = 43,              /* PERPRIMITIVE  */
  YYSYMBOL_STRUCT = 44,                    /* STRUCT  */
  YYSYMBOL_VOID_TOK = 45,                  /* VOID_TOK  */
  YYSYMBOL_WHILE = 46,                     /* WHILE  */
  YYSYMBOL_IDENTIFIER = 47,                /* IDENTIFIER  */
  YYSYMBOL_TYPE_IDENTIFIER = 48,           /* TYPE_IDENTIFIER  */
  YYSYMBOL_NEW_IDENTIFIER = 49,            /* NEW_IDENTIFIER  */
  YYSYMBOL_FLOATCONSTANT = 50,             /* FLOATCONSTANT  */
  YYSYMBOL_FLOAT16CONSTANT = 51,           /* FLOAT16CONSTANT  */
  YYSYMBOL_DOUBLECONSTANT = 52,            /* DOUBLECONSTANT  */
  YYSYMBOL_INTCONSTANT = 53,               /* INTCONSTANT  */
  YYSYMBOL_UINTCONSTANT = 54,              /* UINTCONSTANT  */
  YYSYMBOL_BOOLCONSTANT = 55,              /* BOOLCONSTANT  */
  YYSYMBOL_INT64CONSTANT = 56,             /* INT64CONSTANT  */
  YYSYMBOL_UINT64CONSTANT = 57,            /* UINT64CONSTANT  */
  YYSYMBOL_FIELD_SELECTION = 58,           /* FIELD_SELECTION  */
  YYSYMBOL_CSCSTANDARD = 59,               /* CSCSTANDARD  */
  YYSYMBOL_LEFT_OP = 60,                   /* LEFT_OP  */
  YYSYMBOL_RIGHT_OP = 61,                  /* RIGHT_OP  */
  YYSYMBOL_INC_OP = 62,                    /* INC_OP  */
  YYSYMBOL_DEC_OP = 63,                    /* DEC_OP  */
  YYSYMBOL_LE_OP = 64,                     /* LE_OP  */
  YYSYMBOL_GE_OP = 65,                     /* GE_OP  */
  YYSYMBOL_EQ_OP = 66,                     /* EQ_OP  */
  YYSYMBOL_NE_OP = 67,                     /* NE_OP  */
  YYSYMBOL_AND_OP = 68,                    /* AND_OP  */
  YYSYMBOL_OR_OP = 69,                     /* OR_OP  */
  YYSYMBOL_XOR_OP = 70,                    /* XOR_OP  */
  YYSYMBOL_MUL_ASSIGN = 71,                /* MUL_ASSIGN  */
  YYSYMBOL_DIV_ASSIGN = 72,                /* DIV_ASSIGN  */
  YYSYMBOL_ADD_ASSIGN = 73,                /* ADD_ASSIGN  */
  YYSYMBOL_MOD_ASSIGN = 74,                /* MOD_ASSIGN  */
  YYSYMBOL_LEFT_ASSIGN = 75,               /* LEFT_ASSIGN  */
  YYSYMBOL_RIGHT_ASSIGN = 76,              /* RIGHT_ASSIGN  */
  YYSYMBOL_AND_ASSIGN = 77,                /* AND_ASSIGN  */
  YYSYMBOL_XOR_ASSIGN = 78,                /* XOR_ASSIGN  */
  YYSYMBOL_OR_ASSIGN = 79,                 /* OR_ASSIGN  */
  YYSYMBOL_SUB_ASSIGN = 80,                /* SUB_ASSIGN  */
  YYSYMBOL_INVARIANT = 81,                 /* INVARIANT  */
  YYSYMBOL_PRECISE = 82,                   /* PRECISE  */
  YYSYMBOL_LOWP = 83,                      /* LOWP  */
  YYSYMBOL_MEDIUMP = 84,                   /* MEDIUMP  */
  YYSYMBOL_HIGHP = 85,                     /* HIGHP  */
  YYSYMBOL_SUPERP = 86,                    /* SUPERP  */
  YYSYMBOL_PRECISION = 87,                 /* PRECISION  */
  YYSYMBOL_VERSION_TOK = 88,               /* VERSION_TOK  */
  YYSYMBOL_EXTENSION = 89,                 /* EXTENSION  */
  YYSYMBOL_LINE = 90,                      /* LINE  */
  YYSYMBOL_COLON = 91,                     /* COLON  */
  YYSYMBOL_EOL = 92,                       /* EOL  */
  YYSYMBOL_INTERFACE_TOK = 93,             /* INTERFACE_TOK  */
  YYSYMBOL_OUTPUT = 94,                    /* OUTPUT  */
  YYSYMBOL_PRAGMA_DEBUG_ON = 95,           /* PRAGMA_DEBUG_ON  */
  YYSYMBOL_PRAGMA_DEBUG_OFF = 96,          /* PRAGMA_DEBUG_OFF  */
  YYSYMBOL_PRAGMA_OPTIMIZE_ON = 97,        /* PRAGMA_OPTIMIZE_ON  */
  YYSYMBOL_PRAGMA_OPTIMIZE_OFF = 98,       /* PRAGMA_OPTIMIZE_OFF  */
  YYSYMBOL_PRAGMA_WARNING_ON = 99,         /* PRAGMA_WARNING_ON  */
  YYSYMBOL_PRAGMA_WARNING_OFF = 100,       /* PRAGMA_WARNING_OFF  */
  YYSYMBOL_PRAGMA_INVARIANT_ALL = 101,     /* PRAGMA_INVARIANT_ALL  */
  YYSYMBOL_LAYOUT_TOK = 102,               /* LAYOUT_TOK  */
  YYSYMBOL_DOT_TOK = 103,                  /* DOT_TOK  */
  YYSYMBOL_ASM = 104,                      /* ASM  */
  YYSYMBOL_CLASS = 105,                    /* CLASS  */
  YYSYMBOL_UNION = 106,                    /* UNION  */
  YYSYMBOL_ENUM = 107,                     /* ENUM  */
  YYSYMBOL_TYPEDEF = 108,                  /* TYPEDEF  */
  YYSYMBOL_TEMPLATE = 109,                 /* TEMPLATE  */
  YYSYMBOL_THIS = 110,                     /* THIS  */
  YYSYMBOL_PACKED_TOK = 111,               /* PACKED_TOK  */
  YYSYMBOL_GOTO = 112,                     /* GOTO  */
  YYSYMBOL_INLINE_TOK = 113,               /* INLINE_TOK  */
  YYSYMBOL_NOINLINE = 114,                 /* NOINLINE  */
  YYSYMBOL_PUBLIC_TOK = 115,               /* PUBLIC_TOK  */
  YYSYMBOL_STATIC = 116,                   /* STATIC  */
  YYSYMBOL_EXTERN = 117,                   /* EXTERN  */
  YYSYMBOL_EXTERNAL = 118,                 /* EXTERNAL  */
  YYSYMBOL_LONG_TOK = 119,                 /* LONG_TOK  */
  YYSYMBOL_SHORT_TOK = 120,                /* SHORT_TOK  */
  YYSYMBOL_HALF = 121,                     /* HALF  */
  YYSYMBOL_FIXED_TOK = 122,                /* FIXED_TOK  */
  YYSYMBOL_UNSIGNED = 123,                 /* UNSIGNED  */
  YYSYMBOL_INPUT_TOK = 124,                /* INPUT_TOK  */
  YYSYMBOL_HVEC2 = 125,                    /* HVEC2  */
  YYSYMBOL_HVEC3 = 126,                    /* HVEC3  */
  YYSYMBOL_HVEC4 = 127,                    /* HVEC4  */
  YYSYMBOL_FVEC2 = 128,                    /* FVEC2  */
  YYSYMBOL_FVEC3 = 129,                    /* FVEC3  */
  YYSYMBOL_FVEC4 = 130,                    /* FVEC4  */
  YYSYMBOL_SAMPLER3DRECT = 131,            /* SAMPLER3DRECT  */
  YYSYMBOL_SIZEOF = 132,                   /* SIZEOF  */
  YYSYMBOL_CAST = 133,                     /* CAST  */
  YYSYMBOL_NAMESPACE = 134,                /* NAMESPACE  */
  YYSYMBOL_USING = 135,                    /* USING  */
  YYSYMBOL_RESOURCE = 136,                 /* RESOURCE  */
  YYSYMBOL_PATCH = 137,                    /* PATCH  */
  YYSYMBOL_SUBROUTINE = 138,               /* SUBROUTINE  */
  YYSYMBOL_ERROR_TOK = 139,                /* ERROR_TOK  */
  YYSYMBOL_COMMON = 140,                   /* COMMON  */
  YYSYMBOL_PARTITION = 141,                /* PARTITION  */
  YYSYMBOL_ACTIVE = 142,                   /* ACTIVE  */
  YYSYMBOL_FILTER = 143,                   /* FILTER  */
  YYSYMBOL_ROW_MAJOR = 144,                /* ROW_MAJOR  */
  YYSYMBOL_THEN = 145,                     /* THEN  */
  YYSYMBOL_146_ = 146,                     /* '('  */
  YYSYMBOL_147_ = 147,                     /* ')'  */
  YYSYMBOL_148_ = 148,                     /* '['  */
  YYSYMBOL_149_ = 149,                     /* ']'  */
  YYSYMBOL_150_ = 150,                     /* ','  */
  YYSYMBOL_151_ = 151,                     /* '+'  */
  YYSYMBOL_152_ = 152,                     /* '-'  */
  YYSYMBOL_153_ = 153,                     /* '!'  */
  YYSYMBOL_154_ = 154,                     /* '~'  */
  YYSYMBOL_155_ = 155,                     /* '*'  */
  YYSYMBOL_156_ = 156,                     /* '/'  */
  YYSYMBOL_157_ = 157,                     /* '%'  */
  YYSYMBOL_158_ = 158,                     /* '<'  */
  YYSYMBOL_159_ = 159,                     /* '>'  */
  YYSYMBOL_160_ = 160,                     /* '&'  */
  YYSYMBOL_161_ = 161,                     /* '^'  */
  YYSYMBOL_162_ = 162,                     /* '|'  */
  YYSYMBOL_163_ = 163,                     /* '?'  */
  YYSYMBOL_164_ = 164,                     /* ':'  */
  YYSYMBOL_165_ = 165,                     /* '='  */
  YYSYMBOL_166_ = 166,                     /* ';'  */
  YYSYMBOL_167_ = 167,                     /* '{'  */
  YYSYMBOL_168_ = 168,                     /* '}'  */
  YYSYMBOL_YYACCEPT = 169,                 /* $accept  */
  YYSYMBOL_translation_unit = 170,         /* translation_unit  */
  YYSYMBOL_171_1 = 171,                    /* $@1  */
  YYSYMBOL_version_statement = 172,        /* version_statement  */
  YYSYMBOL_pragma_statement = 173,         /* pragma_statement  */
  YYSYMBOL_extension_statement_list = 174, /* extension_statement_list  */
  YYSYMBOL_any_identifier = 175,           /* any_identifier  */
  YYSYMBOL_extension_statement = 176,      /* extension_statement  */
  YYSYMBOL_external_declaration_list = 177, /* external_declaration_list  */
  YYSYMBOL_variable_identifier = 178,      /* variable_identifier  */
  YYSYMBOL_primary_expression = 179,       /* primary_expression  */
  YYSYMBOL_postfix_expression = 180,       /* postfix_expression  */
  YYSYMBOL_integer_expression = 181,       /* integer_expression  */
  YYSYMBOL_function_call = 182,            /* function_call  */
  YYSYMBOL_function_call_or_method = 183,  /* function_call_or_method  */
  YYSYMBOL_function_call_generic = 184,    /* function_call_generic  */
  YYSYMBOL_function_call_header_no_parameters = 185, /* function_call_header_no_parameters  */
  YYSYMBOL_function_call_header_with_parameters = 186, /* function_call_header_with_parameters  */
  YYSYMBOL_function_call_header = 187,     /* function_call_header  */
  YYSYMBOL_function_identifier = 188,      /* function_identifier  */
  YYSYMBOL_unary_expression = 189,         /* unary_expression  */
  YYSYMBOL_unary_operator = 190,           /* unary_operator  */
  YYSYMBOL_multiplicative_expression = 191, /* multiplicative_expression  */
  YYSYMBOL_additive_expression = 192,      /* additive_expression  */
  YYSYMBOL_shift_expression = 193,         /* shift_expression  */
  YYSYMBOL_relational_expression = 194,    /* relational_expression  */
  YYSYMBOL_equality_expression = 195,      /* equality_expression  */
  YYSYMBOL_and_expression = 196,           /* and_expression  */
  YYSYMBOL_exclusive_or_expression = 197,  /* exclusive_or_expression  */
  YYSYMBOL_inclusive_or_expression = 198,  /* inclusive_or_expression  */
  YYSYMBOL_logical_and_expression = 199,   /* logical_and_expression  */
  YYSYMBOL_logical_xor_expression = 200,   /* logical_xor_expression  */
  YYSYMBOL_logical_or_expression = 201,    /* logical_or_expression  */
  YYSYMBOL_conditional_expression = 202,   /* conditional_expression  */
  YYSYMBOL_assignment_expression = 203,    /* assignment_expression  */
  YYSYMBOL_assignment_operator = 204,      /* assignment_operator  */
  YYSYMBOL_expression = 205,               /* expression  */
  YYSYMBOL_constant_expression = 206,      /* constant_expression  */
  YYSYMBOL_declaration = 207,              /* declaration  */
  YYSYMBOL_function_prototype = 208,       /* function_prototype  */
  YYSYMBOL_function_declarator = 209,      /* function_declarator  */
  YYSYMBOL_function_header_with_parameters = 210, /* function_header_with_parameters  */
  YYSYMBOL_function_header = 211,          /* function_header  */
  YYSYMBOL_parameter_declarator = 212,     /* parameter_declarator  */
  YYSYMBOL_parameter_declaration = 213,    /* parameter_declaration  */
  YYSYMBOL_parameter_qualifier = 214,      /* parameter_qualifier  */
  YYSYMBOL_parameter_direction_qualifier = 215, /* parameter_direction_qualifier  */
  YYSYMBOL_parameter_type_specifier = 216, /* parameter_type_specifier  */
  YYSYMBOL_init_declarator_list = 217,     /* init_declarator_list  */
  YYSYMBOL_single_declaration = 218,       /* single_declaration  */
  YYSYMBOL_fully_specified_type = 219,     /* fully_specified_type  */
  YYSYMBOL_layout_qualifier = 220,         /* layout_qualifier  */
  YYSYMBOL_layout_qualifier_id_list = 221, /* layout_qualifier_id_list  */
  YYSYMBOL_layout_qualifier_id = 222,      /* layout_qualifier_id  */
  YYSYMBOL_interface_block_layout_qualifier = 223, /* interface_block_layout_qualifier  */
  YYSYMBOL_subroutine_qualifier = 224,     /* subroutine_qualifier  */
  YYSYMBOL_subroutine_type_list = 225,     /* subroutine_type_list  */
  YYSYMBOL_interpolation_qualifier = 226,  /* interpolation_qualifier  */
  YYSYMBOL_type_qualifier = 227,           /* type_qualifier  */
  YYSYMBOL_auxiliary_storage_qualifier = 228, /* auxiliary_storage_qualifier  */
  YYSYMBOL_storage_qualifier = 229,        /* storage_qualifier  */
  YYSYMBOL_memory_qualifier = 230,         /* memory_qualifier  */
  YYSYMBOL_array_specifier = 231,          /* array_specifier  */
  YYSYMBOL_type_specifier = 232,           /* type_specifier  */
  YYSYMBOL_type_specifier_nonarray = 233,  /* type_specifier_nonarray  */
  YYSYMBOL_basic_type_specifier_nonarray = 234, /* basic_type_specifier_nonarray  */
  YYSYMBOL_precision_qualifier = 235,      /* precision_qualifier  */
  YYSYMBOL_struct_specifier = 236,         /* struct_specifier  */
  YYSYMBOL_struct_declaration_list = 237,  /* struct_declaration_list  */
  YYSYMBOL_struct_declaration = 238,       /* struct_declaration  */
  YYSYMBOL_struct_declarator_list = 239,   /* struct_declarator_list  */
  YYSYMBOL_struct_declarator = 240,        /* struct_declarator  */
  YYSYMBOL_initializer = 241,              /* initializer  */
  YYSYMBOL_initializer_list = 242,         /* initializer_list  */
  YYSYMBOL_declaration_statement = 243,    /* declaration_statement  */
  YYSYMBOL_statement = 244,                /* statement  */
  YYSYMBOL_simple_statement = 245,         /* simple_statement  */
  YYSYMBOL_compound_statement = 246,       /* compound_statement  */
  YYSYMBOL_247_2 = 247,                    /* $@2  */
  YYSYMBOL_statement_no_new_scope = 248,   /* statement_no_new_scope  */
  YYSYMBOL_compound_statement_no_new_scope = 249, /* compound_statement_no_new_scope  */
  YYSYMBOL_statement_list = 250,           /* statement_list  */
  YYSYMBOL_expression_statement = 251,     /* expression_statement  */
  YYSYMBOL_selection_statement = 252,      /* selection_statement  */
  YYSYMBOL_selection_rest_statement = 253, /* selection_rest_statement  */
  YYSYMBOL_condition = 254,                /* condition  */
  YYSYMBOL_switch_statement = 255,         /* switch_statement  */
  YYSYMBOL_switch_body = 256,              /* switch_body  */
  YYSYMBOL_case_label = 257,               /* case_label  */
  YYSYMBOL_case_label_list = 258,          /* case_label_list  */
  YYSYMBOL_case_statement = 259,           /* case_statement  */
  YYSYMBOL_case_statement_list = 260,      /* case_statement_list  */
  YYSYMBOL_iteration_statement = 261,      /* iteration_statement  */
  YYSYMBOL_for_init_statement = 262,       /* for_init_statement  */
  YYSYMBOL_conditionopt = 263,             /* conditionopt  */
  YYSYMBOL_for_rest_statement = 264,       /* for_rest_statement  */
  YYSYMBOL_jump_statement = 265,           /* jump_statement  */
  YYSYMBOL_demote_statement = 266,         /* demote_statement  */
  YYSYMBOL_external_declaration = 267,     /* external_declaration  */
  YYSYMBOL_function_definition = 268,      /* function_definition  */
  YYSYMBOL_interface_block = 269,          /* interface_block  */
  YYSYMBOL_basic_interface_block = 270,    /* basic_interface_block  */
  YYSYMBOL_interface_qualifier = 271,      /* interface_qualifier  */
  YYSYMBOL_instance_name_opt = 272,        /* instance_name_opt  */
  YYSYMBOL_member_list = 273,              /* member_list  */
  YYSYMBOL_member_declaration = 274,       /* member_declaration  */
  YYSYMBOL_layout_uniform_defaults = 275,  /* layout_uniform_defaults  */
  YYSYMBOL_layout_buffer_defaults = 276,   /* layout_buffer_defaults  */
  YYSYMBOL_layout_in_defaults = 277,       /* layout_in_defaults  */
  YYSYMBOL_layout_out_defaults = 278,      /* layout_out_defaults  */
  YYSYMBOL_layout_defaults = 279           /* layout_defaults  */
};
typedef enum yysymbol_kind_t yysymbol_kind_t;




#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

/* Work around bug in HP-UX 11.23, which defines these macros
   incorrectly for preprocessor constants.  This workaround can likely
   be removed in 2023, as HPE has promised support for HP-UX 11.23
   (aka HP-UX 11i v2) only through the end of 2022; see Table 2 of
   <https://h20195.www2.hpe.com/V2/getpdf.aspx/4AA4-7673ENW.pdf>.  */
#ifdef __hpux
# undef UINT_LEAST8_MAX
# undef UINT_LEAST16_MAX
# define UINT_LEAST8_MAX 255
# define UINT_LEAST16_MAX 65535
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))


/* Stored state numbers (used for stacks). */
typedef yytype_int16 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif


#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YY_USE(E) ((void) (E))
#else
# define YY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#if defined __GNUC__ && ! defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
# if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")
# else
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# endif
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if 1

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* 1 */

#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL \
             && defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
  YYLTYPE yyls_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE) \
             + YYSIZEOF (YYLTYPE)) \
      + 2 * YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  5
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   2793

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  169
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  111
/* YYNRULES -- Number of rules.  */
#define YYNRULES  322
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  485

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   400


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK                     \
   ? YY_CAST (yysymbol_kind_t, yytranslate[YYX])        \
   : YYSYMBOL_YYUNDEF)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_uint8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,   153,     2,     2,     2,   157,   160,     2,
     146,   147,   155,   151,   150,   152,     2,   156,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,   164,   166,
     158,   165,   159,   163,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,   148,     2,   149,   161,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,   167,   162,   168,   154,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    58,    59,    60,    61,    62,    63,    64,
      65,    66,    67,    68,    69,    70,    71,    72,    73,    74,
      75,    76,    77,    78,    79,    80,    81,    82,    83,    84,
      85,    86,    87,    88,    89,    90,    91,    92,    93,    94,
      95,    96,    97,    98,    99,   100,   101,   102,   103,   104,
     105,   106,   107,   108,   109,   110,   111,   112,   113,   114,
     115,   116,   117,   118,   119,   120,   121,   122,   123,   124,
     125,   126,   127,   128,   129,   130,   131,   132,   133,   134,
     135,   136,   137,   138,   139,   140,   141,   142,   143,   144,
     145
};

#if YYDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_int16 yyrline[] =
{
       0,   301,   301,   300,   325,   327,   334,   344,   345,   346,
     347,   348,   372,   377,   384,   386,   390,   391,   392,   396,
     405,   413,   421,   432,   433,   437,   444,   451,   458,   465,
     472,   479,   486,   493,   500,   507,   514,   515,   521,   525,
     532,   538,   547,   551,   555,   559,   560,   564,   565,   569,
     575,   587,   591,   597,   611,   612,   618,   624,   634,   635,
     636,   637,   641,   642,   648,   654,   663,   664,   670,   679,
     680,   686,   695,   696,   702,   708,   714,   723,   724,   730,
     739,   740,   749,   750,   759,   760,   769,   770,   779,   780,
     789,   790,   799,   800,   809,   810,   819,   820,   821,   822,
     823,   824,   825,   826,   827,   828,   829,   833,   837,   853,
     857,   862,   866,   871,   888,   892,   893,   897,   902,   910,
     928,   939,   944,   959,   967,   984,   987,   995,  1003,  1015,
    1027,  1034,  1039,  1044,  1053,  1057,  1058,  1068,  1078,  1088,
    1102,  1109,  1120,  1131,  1142,  1153,  1165,  1180,  1187,  1205,
    1212,  1213,  1223,  1786,  1964,  1990,  1995,  2000,  2008,  2013,
    2022,  2031,  2043,  2048,  2053,  2062,  2067,  2072,  2073,  2074,
    2075,  2076,  2077,  2078,  2096,  2104,  2129,  2153,  2167,  2172,
    2188,  2213,  2225,  2233,  2238,  2243,  2248,  2255,  2260,  2265,
    2270,  2275,  2300,  2312,  2317,  2322,  2327,  2332,  2337,  2342,
    2350,  2355,  2360,  2366,  2371,  2379,  2387,  2393,  2403,  2414,
    2415,  2423,  2429,  2435,  2444,  2445,  2446,  2458,  2463,  2468,
    2476,  2483,  2500,  2505,  2513,  2551,  2556,  2564,  2570,  2579,
    2580,  2584,  2591,  2598,  2605,  2611,  2612,  2616,  2617,  2618,
    2619,  2620,  2621,  2622,  2626,  2633,  2632,  2646,  2647,  2651,
    2657,  2666,  2676,  2685,  2697,  2703,  2712,  2721,  2726,  2734,
    2738,  2756,  2764,  2769,  2777,  2782,  2790,  2798,  2806,  2814,
    2822,  2830,  2838,  2845,  2852,  2862,  2863,  2867,  2869,  2875,
    2880,  2889,  2895,  2901,  2907,  2913,  2922,  2931,  2932,  2933,
    2934,  2935,  2939,  2953,  2957,  2970,  2988,  3007,  3012,  3017,
    3022,  3027,  3032,  3037,  3042,  3058,  3061,  3066,  3074,  3079,
    3087,  3111,  3118,  3122,  3129,  3133,  3143,  3152,  3162,  3171,
    3183,  3205,  3215
};
#endif

/** Accessing symbol of state STATE.  */
#define YY_ACCESSING_SYMBOL(State) YY_CAST (yysymbol_kind_t, yystos[State])

#if 1
/* The user-facing name of the symbol whose (internal) number is
   YYSYMBOL.  No bounds checking.  */
static const char *yysymbol_name (yysymbol_kind_t yysymbol) YY_ATTRIBUTE_UNUSED;

/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "\"end of file\"", "error", "\"invalid token\"", "ATTRIBUTE",
  "CONST_TOK", "BASIC_TYPE_TOK", "BREAK", "BUFFER", "CONTINUE", "DO",
  "ELSE", "FOR", "IF", "DEMOTE", "DISCARD", "RETURN", "SWITCH", "CASE",
  "DEFAULT", "CENTROID", "IN_TOK", "OUT_TOK", "INOUT_TOK", "UNIFORM",
  "VARYING", "SAMPLE", "NOPERSPECTIVE", "FLAT", "SMOOTH", "PIXEL_LOCAL",
  "PIXEL_LOCAL_IN", "PIXEL_LOCAL_OUT", "IMAGE1DSHADOW", "IMAGE2DSHADOW",
  "IMAGE1DARRAYSHADOW", "IMAGE2DARRAYSHADOW", "COHERENT", "VOLATILE",
  "RESTRICT", "READONLY", "WRITEONLY", "SHARED", "TASKPAYLOAD",
  "PERPRIMITIVE", "STRUCT", "VOID_TOK", "WHILE", "IDENTIFIER",
  "TYPE_IDENTIFIER", "NEW_IDENTIFIER", "FLOATCONSTANT", "FLOAT16CONSTANT",
  "DOUBLECONSTANT", "INTCONSTANT", "UINTCONSTANT", "BOOLCONSTANT",
  "INT64CONSTANT", "UINT64CONSTANT", "FIELD_SELECTION", "CSCSTANDARD",
  "LEFT_OP", "RIGHT_OP", "INC_OP", "DEC_OP", "LE_OP", "GE_OP", "EQ_OP",
  "NE_OP", "AND_OP", "OR_OP", "XOR_OP", "MUL_ASSIGN", "DIV_ASSIGN",
  "ADD_ASSIGN", "MOD_ASSIGN", "LEFT_ASSIGN", "RIGHT_ASSIGN", "AND_ASSIGN",
  "XOR_ASSIGN", "OR_ASSIGN", "SUB_ASSIGN", "INVARIANT", "PRECISE", "LOWP",
  "MEDIUMP", "HIGHP", "SUPERP", "PRECISION", "VERSION_TOK", "EXTENSION",
  "LINE", "COLON", "EOL", "INTERFACE_TOK", "OUTPUT", "PRAGMA_DEBUG_ON",
  "PRAGMA_DEBUG_OFF", "PRAGMA_OPTIMIZE_ON", "PRAGMA_OPTIMIZE_OFF",
  "PRAGMA_WARNING_ON", "PRAGMA_WARNING_OFF", "PRAGMA_INVARIANT_ALL",
  "LAYOUT_TOK", "DOT_TOK", "ASM", "CLASS", "UNION", "ENUM", "TYPEDEF",
  "TEMPLATE", "THIS", "PACKED_TOK", "GOTO", "INLINE_TOK", "NOINLINE",
  "PUBLIC_TOK", "STATIC", "EXTERN", "EXTERNAL", "LONG_TOK", "SHORT_TOK",
  "HALF", "FIXED_TOK", "UNSIGNED", "INPUT_TOK", "HVEC2", "HVEC3", "HVEC4",
  "FVEC2", "FVEC3", "FVEC4", "SAMPLER3DRECT", "SIZEOF", "CAST",
  "NAMESPACE", "USING", "RESOURCE", "PATCH", "SUBROUTINE", "ERROR_TOK",
  "COMMON", "PARTITION", "ACTIVE", "FILTER", "ROW_MAJOR", "THEN", "'('",
  "')'", "'['", "']'", "','", "'+'", "'-'", "'!'", "'~'", "'*'", "'/'",
  "'%'", "'<'", "'>'", "'&'", "'^'", "'|'", "'?'", "':'", "'='", "';'",
  "'{'", "'}'", "$accept", "translation_unit", "$@1", "version_statement",
  "pragma_statement", "extension_statement_list", "any_identifier",
  "extension_statement", "external_declaration_list",
  "variable_identifier", "primary_expression", "postfix_expression",
  "integer_expression", "function_call", "function_call_or_method",
  "function_call_generic", "function_call_header_no_parameters",
  "function_call_header_with_parameters", "function_call_header",
  "function_identifier", "unary_expression", "unary_operator",
  "multiplicative_expression", "additive_expression", "shift_expression",
  "relational_expression", "equality_expression", "and_expression",
  "exclusive_or_expression", "inclusive_or_expression",
  "logical_and_expression", "logical_xor_expression",
  "logical_or_expression", "conditional_expression",
  "assignment_expression", "assignment_operator", "expression",
  "constant_expression", "declaration", "function_prototype",
  "function_declarator", "function_header_with_parameters",
  "function_header", "parameter_declarator", "parameter_declaration",
  "parameter_qualifier", "parameter_direction_qualifier",
  "parameter_type_specifier", "init_declarator_list", "single_declaration",
  "fully_specified_type", "layout_qualifier", "layout_qualifier_id_list",
  "layout_qualifier_id", "interface_block_layout_qualifier",
  "subroutine_qualifier", "subroutine_type_list",
  "interpolation_qualifier", "type_qualifier",
  "auxiliary_storage_qualifier", "storage_qualifier", "memory_qualifier",
  "array_specifier", "type_specifier", "type_specifier_nonarray",
  "basic_type_specifier_nonarray", "precision_qualifier",
  "struct_specifier", "struct_declaration_list", "struct_declaration",
  "struct_declarator_list", "struct_declarator", "initializer",
  "initializer_list", "declaration_statement", "statement",
  "simple_statement", "compound_statement", "$@2",
  "statement_no_new_scope", "compound_statement_no_new_scope",
  "statement_list", "expression_statement", "selection_statement",
  "selection_rest_statement", "condition", "switch_statement",
  "switch_body", "case_label", "case_label_list", "case_statement",
  "case_statement_list", "iteration_statement", "for_init_statement",
  "conditionopt", "for_rest_statement", "jump_statement",
  "demote_statement", "external_declaration", "function_definition",
  "interface_block", "basic_interface_block", "interface_qualifier",
  "instance_name_opt", "member_list", "member_declaration",
  "layout_uniform_defaults", "layout_buffer_defaults",
  "layout_in_defaults", "layout_out_defaults", "layout_defaults", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-373)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-304)

#define yytable_value_is_error(Yyn) \
  0

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int16 yypact[] =
{
     -34,    15,    90,  -373,   -11,  -373,     8,  -373,  -373,  -373,
    -373,    18,    -2,  1801,  -373,  -373,    32,  -373,  -373,  -373,
      77,  -373,   112,   114,  -373,   116,  -373,  -373,  -373,  -373,
    -373,   129,   134,   145,  -373,  -373,  -373,  -373,  -373,  -373,
    -373,  -373,   -32,  -373,  -373,  2450,  2450,  -373,  -373,  -373,
      17,    27,   107,   118,   120,   130,   140,   142,   100,   211,
    -373,   102,  -373,  -373,  1696,  -373,   -17,   111,   110,   308,
     -84,  -373,   125,  2519,  2586,  2586,    47,  2655,  2586,  2655,
    -373,   119,  -373,  2586,  -373,  -373,  -373,  -373,  -373,   220,
    -373,  -373,  -373,  -373,  -373,    -2,  2383,   104,  -373,  -373,
    -373,  -373,  -373,  -373,  -373,  -373,  -373,  2586,  2586,  -373,
    2586,  -373,  2586,  2586,  -373,  -373,    47,  -373,  -373,  -373,
    -373,  -373,  -373,  -373,    58,  -373,    -2,  -373,  -373,  -373,
     835,  -373,  -373,   560,   560,  -373,  -373,  -373,   560,  -373,
      28,   560,   560,   560,    -2,  -373,   135,   136,  -108,   137,
     -27,   -24,   -22,   -18,  -373,  -373,  -373,  -373,  -373,  -373,
    -373,  -373,  -373,  -373,  -373,  -373,  2655,  -373,  -373,  2133,
     141,  -373,   117,   193,    -2,   923,  -373,  2383,   122,  -373,
    -373,  -373,   127,   -39,  -373,  -373,  -373,    42,   132,   138,
    1291,   144,   147,   139,   143,  1925,   149,   150,  -373,  -373,
    -373,  -373,  -373,  -373,  -373,  -373,  -373,  2213,  2213,  2213,
    -373,  -373,  -373,  -373,  -373,   148,  -373,  -373,  -373,   124,
    -373,  -373,  -373,   155,    51,  2247,   161,   496,  2213,    98,
      91,   178,   -45,   190,   157,   164,   156,   242,   241,   -48,
    -373,  -373,   -73,  -373,   160,   185,  -373,  -373,  -373,  -373,
     503,  -373,  -373,  -373,  -373,  -373,  -373,  -373,  -373,  -373,
    -373,  -373,    47,    -2,  -373,  -373,  -373,  -107,  1901,  -106,
    -373,  -373,  -373,  -373,  -373,  -373,  -373,  -373,   183,  -373,
    2188,  2383,  -373,   119,   -72,  -373,  -373,  -373,   990,  -373,
    2213,  -373,    58,  -373,    -2,  -373,  -373,  -373,   281,  -373,
    1608,  2213,  -373,  -373,  -373,   -62,  2213,  2077,  -373,  -373,
      56,  -373,  1456,  -373,  -373,   275,  2213,  -373,  -373,  2213,
     187,  -373,  -373,  -373,  -373,  -373,  -373,  -373,  -373,  -373,
    -373,  -373,  -373,  -373,  2213,  -373,  2213,  2213,  2213,  2213,
    2213,  2213,  2213,  2213,  2213,  2213,  2213,  2213,  2213,  2213,
    2213,  2213,  2213,  2213,  2213,  2213,  2213,  -373,  -373,  -373,
    -373,    -2,   119,  1901,  -104,  1901,  -373,  -373,  1901,  -373,
    -373,   188,    -2,   170,  2383,   141,    -2,  -373,  -373,  -373,
    -373,  -373,   194,  -373,  -373,  2077,    57,  -373,    70,   191,
      -2,   195,  -373,   669,  -373,   200,   191,  -373,  -373,  -373,
    -373,  -373,    98,    98,    91,    91,   178,   178,   178,   178,
     -45,   -45,   190,   157,   164,   156,   242,   241,  -115,  -373,
    -373,   141,  -373,  1901,  -373,  -118,  -373,  -373,   -54,   294,
    -373,  -373,  2213,  -373,   184,   204,  1456,   186,   189,  1291,
    -373,  -373,  2213,  -373,  1860,  -373,  -373,   119,   196,    86,
    2213,  1291,   342,  -373,   -12,  -373,  1901,  -373,  -373,  -373,
    -373,   141,  -373,   205,   191,  -373,  1456,  2213,   202,  -373,
    -373,  1126,  1456,    -8,  -373,  -373,  -373,  -102,  -373,  -373,
    -373,  -373,  -373,  1456,  -373
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_int16 yydefact[] =
{
       4,     0,     0,    14,     0,     1,     2,    16,    17,    18,
       5,     0,     0,     0,    15,     6,     0,   188,   187,   215,
     194,   183,   190,   191,   192,   193,   189,   184,   164,   163,
     162,   197,   198,   199,   200,   201,   202,   203,   204,   195,
     196,   186,     0,   214,   213,   165,   166,   219,   218,   217,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     185,   158,   291,   289,     3,   288,     0,     0,   116,   125,
       0,   135,   140,   170,   172,   169,     0,   167,   168,   171,
     147,   209,   211,   173,   212,    20,   287,   113,   293,     0,
     319,   320,   321,   322,   290,     0,     0,     0,   194,   190,
     191,   193,   197,   198,   199,    23,    24,   165,   166,   145,
     170,   175,   167,   171,   146,   174,     0,     7,     8,     9,
      10,    12,    13,    11,     0,   216,     0,    22,    21,   110,
       0,   292,   114,   125,   125,   131,   132,   133,   125,   117,
       0,   125,   125,   125,     0,   111,    16,    18,   141,     0,
     194,   190,   191,   193,   177,   294,   311,   313,   315,   317,
     178,   176,   148,   179,   304,   180,   170,   182,   295,     0,
     210,   181,     0,     0,     0,     0,   222,     0,     0,   157,
     156,   155,   152,     0,   150,   154,   160,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,    31,    30,
      32,    26,    27,    33,    28,    29,    34,     0,     0,     0,
      58,    59,    60,    61,   254,   245,   249,    25,    36,    54,
      38,    43,    44,     0,     0,    48,     0,    62,     0,    66,
      69,    72,    77,    80,    82,    84,    86,    88,    90,    92,
      94,   107,     0,   234,     0,   147,   237,   251,   236,   235,
       0,   238,   239,   240,   241,   242,   243,   118,   126,   127,
     123,   124,     0,   134,   128,   130,   129,   136,     0,   142,
     119,   314,   316,   318,   312,   205,    62,   109,     0,    52,
       0,     0,    19,   227,     0,   225,   221,   223,     0,   112,
       0,   149,     0,   159,     0,   282,   281,   248,     0,   247,
       0,     0,   286,   285,   283,     0,     0,     0,    55,    56,
       0,   244,     0,    40,    41,     0,     0,    46,    45,     0,
     214,    49,    51,    97,    98,   100,    99,   102,   103,   104,
     105,   106,   101,    96,     0,    57,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,   255,   250,   253,
     252,     0,   120,     0,   137,     0,   229,   144,     0,   206,
     207,     0,     0,     0,   308,   228,     0,   224,   220,   153,
     151,   161,     0,   276,   275,   278,     0,   284,     0,   259,
       0,     0,    35,     0,    39,     0,    42,    50,    95,    63,
      64,    65,    67,    68,    70,    71,    75,    76,    73,    74,
      78,    79,    81,    83,    85,    87,    89,    91,     0,   108,
     121,   122,   139,     0,   232,     0,   143,   208,     0,   305,
     309,   226,     0,   277,     0,     0,     0,     0,     0,     0,
     246,    37,     0,   138,     0,   230,   310,   306,     0,     0,
     279,     0,   258,   256,     0,   261,     0,   272,    93,   231,
     233,   307,   296,     0,   280,   274,     0,     0,     0,   262,
     266,     0,   270,     0,   260,   273,   257,     0,   265,   268,
     267,   269,   263,   271,   264
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -373,  -373,  -373,  -373,  -373,  -373,    14,     5,  -373,   -33,
    -373,  -373,  -373,  -373,  -373,  -373,  -373,  -373,  -373,  -373,
     151,  -373,   -77,   -76,  -191,   -68,     6,     7,     9,    22,
      23,    16,  -373,  -162,  -197,  -373,  -111,  -237,    11,    21,
    -373,  -373,  -373,  -373,   244,    87,  -373,  -373,  -373,  -373,
     -88,     1,  -373,    89,  -373,  -373,  -373,  -373,   290,   101,
    -373,    -9,  -147,   -13,  -373,  -373,   181,  -373,   201,  -145,
      13,    10,  -183,  -373,    83,  -248,  -187,  -373,  -373,  -372,
     321,    76,    95,  -373,  -373,    20,  -373,  -373,   -75,  -373,
     -74,  -373,  -373,  -373,  -373,  -373,  -373,  -373,   337,  -373,
     -50,  -373,   329,  -373,    33,  -373,   336,   338,   339,   340,
    -373
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int16 yydefgoto[] =
{
       0,     2,    13,     3,    63,     6,   283,   359,    64,   217,
     218,   219,   395,   220,   221,   222,   223,   224,   225,   226,
     227,   228,   229,   230,   231,   232,   233,   234,   235,   236,
     237,   238,   239,   240,   241,   334,   242,   278,   243,   244,
      67,    68,    69,   260,   139,   140,   141,   261,    70,    71,
      72,   110,   183,   184,   185,    74,   187,    75,    76,    77,
      78,   113,   170,   279,    81,    82,    83,    84,   175,   176,
     284,   285,   367,   425,   246,   247,   248,   249,   312,   298,
     299,   250,   251,   252,   453,   391,   253,   455,   470,   471,
     472,   473,   254,   385,   434,   435,   255,   256,    85,    86,
      87,    88,    89,   448,   373,   374,    90,    91,    92,    93,
      94
};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int16 yytable[] =
{
      80,   269,   360,   297,    79,   467,   468,   277,   174,   467,
     468,    14,   109,   114,    73,     7,     8,     9,    11,   343,
     344,   354,  -300,   155,    65,  -297,    16,  -298,   321,   168,
     287,  -299,   444,    19,    66,   356,     7,     8,     9,   149,
     169,   169,   280,   371,   280,     7,     8,     9,   356,   442,
     445,    80,    19,   379,     1,    79,    97,   268,   363,   368,
     142,   423,   484,   162,    79,    73,   144,   457,     4,   127,
      79,   366,    42,    43,    73,    65,    44,   356,   376,   465,
     166,    10,   145,    80,   305,    66,   148,   174,   356,   174,
       5,    42,    43,   357,   377,    44,   376,    12,   310,   179,
      47,    48,    49,   178,   387,     7,     8,     9,   291,   173,
      15,   292,   446,   345,   346,   355,   155,   245,   277,   117,
     364,    79,   397,    95,   142,   142,  -300,   263,   277,   142,
      58,   166,   142,   142,   142,    96,   375,   398,   182,   271,
     186,   262,   272,   287,   273,   360,   112,   112,   274,   129,
     130,    59,   406,   407,   408,   409,   469,    79,   267,   419,
     482,  -297,    80,  -298,    80,  -299,   366,   166,   366,   180,
      59,   366,   146,     8,   147,   112,   112,   245,  -301,   112,
     422,    79,   424,  -302,   112,   426,   313,   314,   452,   293,
     386,   166,   294,   372,  -303,   388,   389,   112,   318,   118,
     174,   319,   181,   392,   436,   396,   356,   356,   112,   112,
     119,   112,   120,   112,   112,   421,   125,   437,   476,   390,
     356,   258,   121,   479,   481,   259,   366,   315,   264,   265,
     266,   116,   122,   463,   123,   481,   356,   245,   341,   342,
     443,    79,   339,   340,   418,   458,   124,   366,   126,   361,
     143,   166,   297,   336,   337,   338,   347,   348,   132,   366,
     133,   460,   402,   403,   297,   404,   405,   169,    80,   172,
     -53,   177,   316,   474,   389,    80,   112,   362,   112,   410,
     411,   -23,   -24,   270,   281,   282,   372,   245,   289,   280,
     300,    79,   290,   301,   245,   306,   307,   390,   295,   245,
     461,   166,   317,    79,   296,   302,   182,   322,   381,   303,
     352,   353,   134,   166,   143,   143,   311,   349,   351,   143,
     276,   449,   143,   143,   143,   350,   129,   382,   135,   136,
     137,   -52,   369,   394,   -47,   111,   115,   427,   429,   464,
     432,   356,   439,   447,    34,    35,    36,    37,    38,   441,
     450,   451,   466,   454,   456,   412,   477,   413,   308,   309,
     414,    80,   462,   154,   160,   161,   478,   163,   165,   167,
     417,   475,   245,   171,   415,   420,   416,   257,   288,   335,
     245,   380,   112,   383,    79,   428,   431,   131,   393,   112,
     138,    47,    48,    49,   166,   384,   480,   111,   115,   483,
     154,   128,   163,   167,   438,   433,   164,   430,   112,   156,
       0,   157,   158,   159,     0,     0,     0,     0,     0,     0,
       0,     0,     0,   245,     0,     0,   245,    79,     0,     0,
      79,   276,     0,     0,     0,     0,     0,   166,   245,     0,
     166,   276,    79,     0,     0,     0,     0,     0,     0,     0,
       0,     0,   166,   245,     0,  -115,   154,    79,   245,   245,
       0,     0,    79,    79,     0,     0,     0,   166,     0,     0,
     245,     0,   166,   166,    79,   112,     0,     0,     0,     0,
       0,     0,     0,     0,   166,     0,   112,   399,   400,   401,
     276,   276,   276,   276,   276,   276,   276,   276,   276,   276,
     276,   276,   276,   276,   276,   276,    17,    18,    19,   188,
      20,   189,   190,     0,   191,   192,   193,   194,   195,   196,
       0,     0,    21,    22,    23,    24,    25,    26,    27,    28,
      29,    30,    31,    32,    33,     0,     0,     0,     0,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,   197,
     105,    44,   106,   198,   199,   200,   201,   202,   203,   204,
     205,     0,   206,     0,   134,   207,   208,   323,   324,   325,
     326,   327,   328,   329,   330,   331,   332,     0,     0,     0,
     135,   136,   137,     0,    45,    46,    47,    48,    49,     0,
      50,     0,    12,     0,     0,     0,    34,    35,    36,    37,
      38,     0,     0,     0,     0,    58,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,    59,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
      60,    61,   138,    47,    48,    49,     0,     0,     0,   209,
       0,     0,     0,     0,   210,   211,   212,   213,     0,     0,
       0,   333,     0,     0,     0,     0,     0,     0,     0,   214,
     215,   358,    17,    18,    19,   188,    20,   189,   190,     0,
     191,   192,   193,   194,   195,   196,     0,     0,    21,    22,
      23,    24,    25,    26,    27,    28,    29,    30,    31,    32,
      33,     0,     0,     0,     0,    34,    35,    36,    37,    38,
      39,    40,    41,    42,    43,   197,   105,    44,   106,   198,
     199,   200,   201,   202,   203,   204,   205,     0,   206,     0,
       0,   207,   208,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
      45,    46,    47,    48,    49,     0,    50,     0,    12,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,    58,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,    59,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,    60,    61,     0,     0,
       0,     0,     0,     0,     0,   209,     0,     0,     0,     0,
     210,   211,   212,   213,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,   214,   215,   440,    17,    18,
      19,   188,    20,   189,   190,     0,   191,   192,   193,   194,
     195,   196,     0,     0,    21,    22,    23,    24,    25,    26,
      27,    28,    29,    30,    31,    32,    33,     0,     0,     0,
       0,    34,    35,    36,    37,    38,    39,    40,    41,    42,
      43,   197,   105,    44,   106,   198,   199,   200,   201,   202,
     203,   204,   205,     0,   206,     0,     0,   207,   208,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,    45,    46,    47,    48,
      49,     0,    50,     0,     0,     0,    17,    18,    19,     0,
      98,     0,     0,     0,     0,     0,     0,    58,     0,     0,
       0,     0,    21,    99,   100,    24,   101,    26,    27,    28,
      29,    30,   102,   103,   104,     0,     0,     0,    59,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,     0,
       0,    44,    60,    61,     0,     0,     0,     0,     0,     0,
       0,   209,     0,     0,     0,     0,   210,   211,   212,   213,
       0,     0,     0,    17,    18,    19,     0,    98,     0,     0,
       0,   214,   215,   216,   107,   108,    47,    48,    49,    21,
      99,   100,    24,   101,    26,    27,    28,    29,    30,   102,
     103,   104,     0,     0,     0,    58,    34,    35,    36,    37,
      38,    39,    40,    41,    42,    43,     0,     0,    44,     0,
       0,     0,     0,     0,     0,     0,    59,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
      60,    61,     0,     0,     0,     0,     0,     0,     0,     0,
       0,   107,   108,    47,    48,    49,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,   286,    58,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,    59,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    60,    61,    17,
      18,    19,   188,    20,   189,   190,     0,   191,   192,   193,
     194,   195,   196,   467,   468,    21,    22,    23,    24,    25,
      26,    27,    28,    29,    30,    31,    32,    33,   378,     0,
       0,     0,    34,    35,    36,    37,    38,    39,    40,    41,
      42,    43,   197,   105,    44,   106,   198,   199,   200,   201,
     202,   203,   204,   205,     0,   206,     0,     0,   207,   208,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    45,    46,    47,
      48,    49,     0,    50,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,    58,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    59,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,    60,    61,     0,     0,     0,     0,     0,
       0,     0,   209,     0,     0,     0,     0,   210,   211,   212,
     213,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,   214,   215,    17,    18,    19,   188,    20,   189,
     190,     0,   191,   192,   193,   194,   195,   196,     0,     0,
      21,    22,    23,    24,    25,    26,    27,    28,    29,    30,
      31,    32,    33,     0,     0,     0,     0,    34,    35,    36,
      37,    38,    39,    40,    41,    42,    43,   197,   105,    44,
     106,   198,   199,   200,   201,   202,   203,   204,   205,     0,
     206,     0,     0,   207,   208,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,    45,    46,    47,    48,    49,     0,    50,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,    58,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,    59,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,    60,    61,
       0,     0,     0,     0,     0,     0,     0,   209,     0,     0,
       0,     0,   210,   211,   212,   213,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,   214,   130,    17,
      18,    19,   188,    20,   189,   190,     0,   191,   192,   193,
     194,   195,   196,     0,     0,    21,    22,    23,    24,    25,
      26,    27,    28,    29,    30,    31,    32,    33,     0,     0,
       0,     0,    34,    35,    36,    37,    38,    39,    40,    41,
      42,    43,   197,   105,    44,   106,   198,   199,   200,   201,
     202,   203,   204,   205,     0,   206,     0,     0,   207,   208,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    45,    46,    47,
      48,    49,     0,    50,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,    58,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    59,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,    60,    61,     0,     0,     0,     0,     0,
       0,     0,   209,     0,     0,     0,     0,   210,   211,   212,
     213,    17,    18,    19,     0,    20,     0,     0,     0,     0,
       0,     0,   214,   215,     0,     0,     0,    21,    22,    23,
      24,    25,    26,    27,    28,    29,    30,    31,    32,    33,
       0,     0,     0,     0,    34,    35,    36,    37,    38,    39,
      40,    41,    42,    43,     0,   105,    44,   106,   198,   199,
     200,   201,   202,   203,   204,   205,     0,   206,     0,     0,
     207,   208,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    45,
      46,    47,    48,    49,     0,    50,     0,     0,     0,    17,
      18,    19,     0,    20,     0,     0,     0,     0,     0,     0,
      58,     0,     0,     0,     0,    21,    22,    23,    24,    25,
      26,    27,    28,    29,    30,    31,    32,    33,     0,     0,
       0,    59,    34,    35,    36,    37,    38,    39,    40,    41,
      42,    43,     0,     0,    44,    60,    61,     0,     0,     0,
       0,     0,     0,     0,   209,     0,     0,     0,     0,   210,
     211,   212,   213,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,   214,     0,     0,    45,    46,    47,
      48,    49,     0,    50,     0,    12,     0,     0,     0,     0,
       0,    51,    52,    53,    54,    55,    56,    57,    58,     0,
       0,     0,     0,     0,    17,    18,    19,     0,    20,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    59,
      21,    22,    23,    24,    25,    26,    27,    28,    29,    30,
      31,    32,    33,    60,    61,     0,     0,    34,    35,    36,
      37,    38,    39,    40,    41,    42,    43,     0,     0,    44,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,    62,     0,     0,    19,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,    45,    46,    47,    48,    49,     0,    50,     0,
       0,     0,     0,     0,     0,     0,    51,    52,    53,    54,
      55,    56,    57,    58,    42,    43,    19,   105,    44,   106,
     198,   199,   200,   201,   202,   203,   204,   205,     0,   206,
       0,     0,   207,   208,    59,     0,     0,     0,     0,     0,
      19,     0,     0,     0,     0,     0,     0,     0,    60,    61,
       0,     0,     0,     0,     0,    42,    43,     0,   105,    44,
     106,   198,   199,   200,   201,   202,   203,   204,   205,     0,
     206,     0,     0,   207,   208,     0,     0,    62,     0,    42,
      43,     0,   105,    44,   106,   198,   199,   200,   201,   202,
     203,   204,   205,    59,   206,     0,     0,   207,   208,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,   209,     0,     0,     0,
       0,   210,   211,   212,   213,     0,     0,     0,     0,     0,
       0,     0,     0,     0,    59,     0,     0,   365,   459,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,   209,    59,     0,
       0,     0,   210,   211,   212,   213,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,   365,     0,
       0,   209,     0,     0,     0,     0,   210,   211,   212,   213,
      17,    18,    19,     0,    98,     0,     0,     0,     0,     0,
       0,   304,     0,     0,     0,     0,    21,    99,   100,    24,
     101,    26,    27,    28,    29,    30,   102,   103,   104,     0,
       0,     0,     0,    34,    35,    36,    37,    38,    39,    40,
      41,    42,    43,     0,   105,    44,   106,   198,   199,   200,
     201,   202,   203,   204,   205,     0,   206,     0,    19,   207,
     208,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,   107,   108,
      47,    48,    49,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    42,    43,    58,
     105,    44,   106,   198,   199,   200,   201,   202,   203,   204,
     205,     0,   206,    19,     0,   207,   208,     0,     0,     0,
      59,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,    60,    61,     0,     0,    19,     0,
       0,     0,     0,   209,     0,     0,     0,     0,   210,   211,
     212,   213,    42,    43,     0,   105,    44,   106,   198,   199,
     200,   201,   202,   203,   204,   205,     0,   206,     0,     0,
     207,   208,    19,     0,     0,     0,    59,    42,    43,     0,
     105,    44,   106,   198,   199,   200,   201,   202,   203,   204,
     205,     0,   206,     0,     0,   207,   208,     0,     0,   209,
       0,     0,   275,     0,   210,   211,   212,   213,     0,     0,
       0,    42,   320,     0,   105,    44,   106,   198,   199,   200,
     201,   202,   203,   204,   205,     0,   206,     0,     0,   207,
     208,    59,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,   209,     0,    59,   370,     0,   210,
     211,   212,   213,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,   209,
       0,     0,     0,     0,   210,   211,   212,   213,     0,     0,
      59,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,    17,    18,    19,     0,
      98,     0,     0,   209,     0,     0,     0,     0,   210,   211,
     212,   213,    21,    99,   100,    24,   101,    26,    27,    28,
      29,    30,   102,   103,   104,     0,     0,     0,     0,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,     0,
       0,    44,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,    17,    18,     0,     0,    98,     0,     0,
       0,     0,     0,     0,   107,   108,    47,    48,    49,    21,
      99,   100,    24,   101,    26,    27,    28,    29,    30,   102,
     103,   104,     0,     0,     0,    58,    34,    35,    36,    37,
      38,    39,    40,    41,     0,     0,     0,   105,     0,   106,
       0,     0,     0,     0,     0,     0,    59,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
      60,    61,    17,    18,     0,     0,   150,     0,     0,     0,
       0,   107,   108,    47,    48,    49,     0,     0,    21,   151,
     152,    24,   153,    26,    27,    28,    29,    30,    31,    32,
      33,     0,    58,     0,     0,    34,    35,    36,    37,    38,
      39,    40,    41,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    60,    61,    17,
      18,     0,     0,    98,     0,     0,     0,     0,     0,     0,
     107,   108,    47,    48,    49,    21,    99,   100,    24,   101,
      26,    27,    28,    29,    30,   102,   103,   104,     0,     0,
       0,    58,    34,    35,    36,    37,    38,    39,    40,    41,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,    60,    61,    17,    18,
       0,     0,    20,     0,     0,     0,     0,   107,   108,    47,
      48,    49,     0,     0,    21,    22,    23,    24,    25,    26,
      27,    28,    29,    30,    31,    32,    33,     0,    58,     0,
       0,    34,    35,    36,    37,    38,    39,    40,    41,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,    60,    61,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,   107,   108,    47,    48,
      49,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    58,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,    60,    61
};

static const yytype_int16 yycheck[] =
{
      13,   148,   250,   190,    13,    17,    18,   169,    96,    17,
      18,     6,    45,    46,    13,    47,    48,    49,     4,    64,
      65,    69,    49,    73,    13,    49,    12,    49,   225,    79,
     175,    49,   150,     5,    13,   150,    47,    48,    49,    72,
     148,   148,   148,   280,   148,    47,    48,    49,   150,   164,
     168,    64,     5,   290,    88,    64,    42,   165,   165,   165,
      69,   165,   164,    76,    73,    64,   150,   439,    53,    64,
      79,   268,    44,    45,    73,    64,    48,   150,   150,   451,
      79,    92,   166,    96,   195,    64,    72,   175,   150,   177,
       0,    44,    45,   166,   166,    48,   150,    89,   209,    41,
      83,    84,    85,   116,   166,    47,    48,    49,   147,    95,
      92,   150,   166,   158,   159,   163,   166,   130,   280,    92,
     267,   130,   319,    91,   133,   134,    49,   140,   290,   138,
     102,   130,   141,   142,   143,   167,   283,   334,   124,   166,
     126,   140,   166,   288,   166,   393,    45,    46,   166,   166,
     167,   123,   343,   344,   345,   346,   168,   166,   144,   356,
     168,    49,   175,    49,   177,    49,   363,   166,   365,   111,
     123,   368,    47,    48,    49,    74,    75,   190,    49,    78,
     363,   190,   365,    49,    83,   368,    62,    63,   436,   147,
     301,   190,   150,   281,    49,   306,   307,    96,   147,    92,
     288,   150,   144,   147,   147,   316,   150,   150,   107,   108,
      92,   110,    92,   112,   113,   362,     5,   147,   466,   307,
     150,   134,    92,   471,   472,   138,   423,   103,   141,   142,
     143,    50,    92,   147,    92,   483,   150,   250,    60,    61,
     423,   250,   151,   152,   355,   442,   146,   444,   146,   262,
      69,   250,   439,   155,   156,   157,    66,    67,   147,   456,
     150,   444,   339,   340,   451,   341,   342,   148,   281,    49,
     146,   167,   148,   456,   385,   288,   175,   263,   177,   347,
     348,   146,   146,   146,   167,    92,   374,   300,   166,   148,
     146,   300,   165,   146,   307,   146,   146,   385,   166,   312,
     447,   300,   147,   312,   166,   166,   292,   146,   294,   166,
      68,    70,     4,   312,   133,   134,   168,   160,   162,   138,
     169,   432,   141,   142,   143,   161,   166,    46,    20,    21,
      22,   146,   149,    58,   147,    45,    46,   149,   168,   450,
     146,   150,   147,    49,    36,    37,    38,    39,    40,   149,
     166,   147,    10,   167,   165,   349,   467,   350,   207,   208,
     351,   374,   166,    73,    74,    75,   164,    77,    78,    79,
     354,   166,   385,    83,   352,   361,   353,   133,   177,   228,
     393,   292,   281,   300,   393,   372,   376,    66,   312,   288,
      82,    83,    84,    85,   393,   300,   471,   107,   108,   473,
     110,    64,   112,   113,   390,   385,    77,   374,   307,    73,
      -1,    73,    73,    73,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,   436,    -1,    -1,   439,   436,    -1,    -1,
     439,   280,    -1,    -1,    -1,    -1,    -1,   436,   451,    -1,
     439,   290,   451,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,   451,   466,    -1,   147,   166,   466,   471,   472,
      -1,    -1,   471,   472,    -1,    -1,    -1,   466,    -1,    -1,
     483,    -1,   471,   472,   483,   374,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,   483,    -1,   385,   336,   337,   338,
     339,   340,   341,   342,   343,   344,   345,   346,   347,   348,
     349,   350,   351,   352,   353,   354,     3,     4,     5,     6,
       7,     8,     9,    -1,    11,    12,    13,    14,    15,    16,
      -1,    -1,    19,    20,    21,    22,    23,    24,    25,    26,
      27,    28,    29,    30,    31,    -1,    -1,    -1,    -1,    36,
      37,    38,    39,    40,    41,    42,    43,    44,    45,    46,
      47,    48,    49,    50,    51,    52,    53,    54,    55,    56,
      57,    -1,    59,    -1,     4,    62,    63,    71,    72,    73,
      74,    75,    76,    77,    78,    79,    80,    -1,    -1,    -1,
      20,    21,    22,    -1,    81,    82,    83,    84,    85,    -1,
      87,    -1,    89,    -1,    -1,    -1,    36,    37,    38,    39,
      40,    -1,    -1,    -1,    -1,   102,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,   123,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
     137,   138,    82,    83,    84,    85,    -1,    -1,    -1,   146,
      -1,    -1,    -1,    -1,   151,   152,   153,   154,    -1,    -1,
      -1,   165,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   166,
     167,   168,     3,     4,     5,     6,     7,     8,     9,    -1,
      11,    12,    13,    14,    15,    16,    -1,    -1,    19,    20,
      21,    22,    23,    24,    25,    26,    27,    28,    29,    30,
      31,    -1,    -1,    -1,    -1,    36,    37,    38,    39,    40,
      41,    42,    43,    44,    45,    46,    47,    48,    49,    50,
      51,    52,    53,    54,    55,    56,    57,    -1,    59,    -1,
      -1,    62,    63,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      81,    82,    83,    84,    85,    -1,    87,    -1,    89,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,   102,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,   123,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,   137,   138,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,   146,    -1,    -1,    -1,    -1,
     151,   152,   153,   154,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,   166,   167,   168,     3,     4,
       5,     6,     7,     8,     9,    -1,    11,    12,    13,    14,
      15,    16,    -1,    -1,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    -1,    -1,    -1,
      -1,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    -1,    59,    -1,    -1,    62,    63,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    81,    82,    83,    84,
      85,    -1,    87,    -1,    -1,    -1,     3,     4,     5,    -1,
       7,    -1,    -1,    -1,    -1,    -1,    -1,   102,    -1,    -1,
      -1,    -1,    19,    20,    21,    22,    23,    24,    25,    26,
      27,    28,    29,    30,    31,    -1,    -1,    -1,   123,    36,
      37,    38,    39,    40,    41,    42,    43,    44,    45,    -1,
      -1,    48,   137,   138,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,   146,    -1,    -1,    -1,    -1,   151,   152,   153,   154,
      -1,    -1,    -1,     3,     4,     5,    -1,     7,    -1,    -1,
      -1,   166,   167,   168,    81,    82,    83,    84,    85,    19,
      20,    21,    22,    23,    24,    25,    26,    27,    28,    29,
      30,    31,    -1,    -1,    -1,   102,    36,    37,    38,    39,
      40,    41,    42,    43,    44,    45,    -1,    -1,    48,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,   123,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
     137,   138,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    81,    82,    83,    84,    85,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,   168,   102,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,   123,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,   137,   138,     3,
       4,     5,     6,     7,     8,     9,    -1,    11,    12,    13,
      14,    15,    16,    17,    18,    19,    20,    21,    22,    23,
      24,    25,    26,    27,    28,    29,    30,    31,   168,    -1,
      -1,    -1,    36,    37,    38,    39,    40,    41,    42,    43,
      44,    45,    46,    47,    48,    49,    50,    51,    52,    53,
      54,    55,    56,    57,    -1,    59,    -1,    -1,    62,    63,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    81,    82,    83,
      84,    85,    -1,    87,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   102,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   123,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,   137,   138,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,   146,    -1,    -1,    -1,    -1,   151,   152,   153,
     154,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,   166,   167,     3,     4,     5,     6,     7,     8,
       9,    -1,    11,    12,    13,    14,    15,    16,    -1,    -1,
      19,    20,    21,    22,    23,    24,    25,    26,    27,    28,
      29,    30,    31,    -1,    -1,    -1,    -1,    36,    37,    38,
      39,    40,    41,    42,    43,    44,    45,    46,    47,    48,
      49,    50,    51,    52,    53,    54,    55,    56,    57,    -1,
      59,    -1,    -1,    62,    63,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    81,    82,    83,    84,    85,    -1,    87,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,   102,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,   123,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   137,   138,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,   146,    -1,    -1,
      -1,    -1,   151,   152,   153,   154,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,   166,   167,     3,
       4,     5,     6,     7,     8,     9,    -1,    11,    12,    13,
      14,    15,    16,    -1,    -1,    19,    20,    21,    22,    23,
      24,    25,    26,    27,    28,    29,    30,    31,    -1,    -1,
      -1,    -1,    36,    37,    38,    39,    40,    41,    42,    43,
      44,    45,    46,    47,    48,    49,    50,    51,    52,    53,
      54,    55,    56,    57,    -1,    59,    -1,    -1,    62,    63,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    81,    82,    83,
      84,    85,    -1,    87,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   102,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   123,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,   137,   138,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,   146,    -1,    -1,    -1,    -1,   151,   152,   153,
     154,     3,     4,     5,    -1,     7,    -1,    -1,    -1,    -1,
      -1,    -1,   166,   167,    -1,    -1,    -1,    19,    20,    21,
      22,    23,    24,    25,    26,    27,    28,    29,    30,    31,
      -1,    -1,    -1,    -1,    36,    37,    38,    39,    40,    41,
      42,    43,    44,    45,    -1,    47,    48,    49,    50,    51,
      52,    53,    54,    55,    56,    57,    -1,    59,    -1,    -1,
      62,    63,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    81,
      82,    83,    84,    85,    -1,    87,    -1,    -1,    -1,     3,
       4,     5,    -1,     7,    -1,    -1,    -1,    -1,    -1,    -1,
     102,    -1,    -1,    -1,    -1,    19,    20,    21,    22,    23,
      24,    25,    26,    27,    28,    29,    30,    31,    -1,    -1,
      -1,   123,    36,    37,    38,    39,    40,    41,    42,    43,
      44,    45,    -1,    -1,    48,   137,   138,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,   146,    -1,    -1,    -1,    -1,   151,
     152,   153,   154,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,   166,    -1,    -1,    81,    82,    83,
      84,    85,    -1,    87,    -1,    89,    -1,    -1,    -1,    -1,
      -1,    95,    96,    97,    98,    99,   100,   101,   102,    -1,
      -1,    -1,    -1,    -1,     3,     4,     5,    -1,     7,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   123,
      19,    20,    21,    22,    23,    24,    25,    26,    27,    28,
      29,    30,    31,   137,   138,    -1,    -1,    36,    37,    38,
      39,    40,    41,    42,    43,    44,    45,    -1,    -1,    48,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,   166,    -1,    -1,     5,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    81,    82,    83,    84,    85,    -1,    87,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    95,    96,    97,    98,
      99,   100,   101,   102,    44,    45,     5,    47,    48,    49,
      50,    51,    52,    53,    54,    55,    56,    57,    -1,    59,
      -1,    -1,    62,    63,   123,    -1,    -1,    -1,    -1,    -1,
       5,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   137,   138,
      -1,    -1,    -1,    -1,    -1,    44,    45,    -1,    47,    48,
      49,    50,    51,    52,    53,    54,    55,    56,    57,    -1,
      59,    -1,    -1,    62,    63,    -1,    -1,   166,    -1,    44,
      45,    -1,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,   123,    59,    -1,    -1,    62,    63,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,   146,    -1,    -1,    -1,
      -1,   151,   152,   153,   154,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,   123,    -1,    -1,   167,   168,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,   146,   123,    -1,
      -1,    -1,   151,   152,   153,   154,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   167,    -1,
      -1,   146,    -1,    -1,    -1,    -1,   151,   152,   153,   154,
       3,     4,     5,    -1,     7,    -1,    -1,    -1,    -1,    -1,
      -1,   166,    -1,    -1,    -1,    -1,    19,    20,    21,    22,
      23,    24,    25,    26,    27,    28,    29,    30,    31,    -1,
      -1,    -1,    -1,    36,    37,    38,    39,    40,    41,    42,
      43,    44,    45,    -1,    47,    48,    49,    50,    51,    52,
      53,    54,    55,    56,    57,    -1,    59,    -1,     5,    62,
      63,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    81,    82,
      83,    84,    85,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    44,    45,   102,
      47,    48,    49,    50,    51,    52,    53,    54,    55,    56,
      57,    -1,    59,     5,    -1,    62,    63,    -1,    -1,    -1,
     123,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,   137,   138,    -1,    -1,     5,    -1,
      -1,    -1,    -1,   146,    -1,    -1,    -1,    -1,   151,   152,
     153,   154,    44,    45,    -1,    47,    48,    49,    50,    51,
      52,    53,    54,    55,    56,    57,    -1,    59,    -1,    -1,
      62,    63,     5,    -1,    -1,    -1,   123,    44,    45,    -1,
      47,    48,    49,    50,    51,    52,    53,    54,    55,    56,
      57,    -1,    59,    -1,    -1,    62,    63,    -1,    -1,   146,
      -1,    -1,   149,    -1,   151,   152,   153,   154,    -1,    -1,
      -1,    44,    45,    -1,    47,    48,    49,    50,    51,    52,
      53,    54,    55,    56,    57,    -1,    59,    -1,    -1,    62,
      63,   123,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,   146,    -1,   123,   149,    -1,   151,
     152,   153,   154,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   146,
      -1,    -1,    -1,    -1,   151,   152,   153,   154,    -1,    -1,
     123,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,     3,     4,     5,    -1,
       7,    -1,    -1,   146,    -1,    -1,    -1,    -1,   151,   152,
     153,   154,    19,    20,    21,    22,    23,    24,    25,    26,
      27,    28,    29,    30,    31,    -1,    -1,    -1,    -1,    36,
      37,    38,    39,    40,    41,    42,    43,    44,    45,    -1,
      -1,    48,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,     3,     4,    -1,    -1,     7,    -1,    -1,
      -1,    -1,    -1,    -1,    81,    82,    83,    84,    85,    19,
      20,    21,    22,    23,    24,    25,    26,    27,    28,    29,
      30,    31,    -1,    -1,    -1,   102,    36,    37,    38,    39,
      40,    41,    42,    43,    -1,    -1,    -1,    47,    -1,    49,
      -1,    -1,    -1,    -1,    -1,    -1,   123,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
     137,   138,     3,     4,    -1,    -1,     7,    -1,    -1,    -1,
      -1,    81,    82,    83,    84,    85,    -1,    -1,    19,    20,
      21,    22,    23,    24,    25,    26,    27,    28,    29,    30,
      31,    -1,   102,    -1,    -1,    36,    37,    38,    39,    40,
      41,    42,    43,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,   137,   138,     3,
       4,    -1,    -1,     7,    -1,    -1,    -1,    -1,    -1,    -1,
      81,    82,    83,    84,    85,    19,    20,    21,    22,    23,
      24,    25,    26,    27,    28,    29,    30,    31,    -1,    -1,
      -1,   102,    36,    37,    38,    39,    40,    41,    42,    43,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,   137,   138,     3,     4,
      -1,    -1,     7,    -1,    -1,    -1,    -1,    81,    82,    83,
      84,    85,    -1,    -1,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    -1,   102,    -1,
      -1,    36,    37,    38,    39,    40,    41,    42,    43,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,   137,   138,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    81,    82,    83,    84,
      85,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,   102,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,   137,   138
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_int16 yystos[] =
{
       0,    88,   170,   172,    53,     0,   174,    47,    48,    49,
      92,   175,    89,   171,   176,    92,   175,     3,     4,     5,
       7,    19,    20,    21,    22,    23,    24,    25,    26,    27,
      28,    29,    30,    31,    36,    37,    38,    39,    40,    41,
      42,    43,    44,    45,    48,    81,    82,    83,    84,    85,
      87,    95,    96,    97,    98,    99,   100,   101,   102,   123,
     137,   138,   166,   173,   177,   207,   208,   209,   210,   211,
     217,   218,   219,   220,   224,   226,   227,   228,   229,   230,
     232,   233,   234,   235,   236,   267,   268,   269,   270,   271,
     275,   276,   277,   278,   279,    91,   167,   175,     7,    20,
      21,    23,    29,    30,    31,    47,    49,    81,    82,   178,
     220,   227,   228,   230,   178,   227,   235,    92,    92,    92,
      92,    92,    92,    92,   146,     5,   146,   176,   267,   166,
     167,   249,   147,   150,     4,    20,    21,    22,    82,   213,
     214,   215,   230,   235,   150,   166,    47,    49,   175,   178,
       7,    20,    21,    23,   227,   269,   275,   276,   277,   278,
     227,   227,   232,   227,   271,   227,   220,   227,   269,   148,
     231,   227,    49,   175,   219,   237,   238,   167,   232,    41,
     111,   144,   175,   221,   222,   223,   175,   225,     6,     8,
       9,    11,    12,    13,    14,    15,    16,    46,    50,    51,
      52,    53,    54,    55,    56,    57,    59,    62,    63,   146,
     151,   152,   153,   154,   166,   167,   168,   178,   179,   180,
     182,   183,   184,   185,   186,   187,   188,   189,   190,   191,
     192,   193,   194,   195,   196,   197,   198,   199,   200,   201,
     202,   203,   205,   207,   208,   232,   243,   244,   245,   246,
     250,   251,   252,   255,   261,   265,   266,   213,   214,   214,
     212,   216,   220,   232,   214,   214,   214,   175,   165,   231,
     146,   166,   166,   166,   166,   149,   189,   202,   206,   232,
     148,   167,    92,   175,   239,   240,   168,   238,   237,   166,
     165,   147,   150,   147,   150,   166,   166,   245,   248,   249,
     146,   146,   166,   166,   166,   205,   146,   146,   189,   189,
     205,   168,   247,    62,    63,   103,   148,   147,   147,   150,
      45,   203,   146,    71,    72,    73,    74,    75,    76,    77,
      78,    79,    80,   165,   204,   189,   155,   156,   157,   151,
     152,    60,    61,    64,    65,   158,   159,    66,    67,   160,
     161,   162,    68,    70,    69,   163,   150,   166,   168,   176,
     244,   232,   175,   165,   231,   167,   203,   241,   165,   149,
     149,   206,   219,   273,   274,   231,   150,   166,   168,   206,
     222,   175,    46,   243,   251,   262,   205,   166,   205,   205,
     219,   254,   147,   250,    58,   181,   205,   203,   203,   189,
     189,   189,   191,   191,   192,   192,   193,   193,   193,   193,
     194,   194,   195,   196,   197,   198,   199,   200,   205,   203,
     175,   231,   241,   165,   241,   242,   241,   149,   239,   168,
     273,   240,   146,   254,   263,   264,   147,   147,   175,   147,
     168,   149,   164,   241,   150,   168,   166,    49,   272,   205,
     166,   147,   244,   253,   167,   256,   165,   248,   203,   168,
     241,   231,   166,   147,   205,   248,    10,    17,    18,   168,
     257,   258,   259,   260,   241,   166,   244,   205,   164,   244,
     257,   244,   168,   259,   164
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_int16 yyr1[] =
{
       0,   169,   171,   170,   172,   172,   172,   173,   173,   173,
     173,   173,   173,   173,   174,   174,   175,   175,   175,   176,
     177,   177,   177,   178,   178,   179,   179,   179,   179,   179,
     179,   179,   179,   179,   179,   179,   180,   180,   180,   180,
     180,   180,   181,   182,   183,   184,   184,   185,   185,   186,
     186,   187,   188,   188,   189,   189,   189,   189,   190,   190,
     190,   190,   191,   191,   191,   191,   192,   192,   192,   193,
     193,   193,   194,   194,   194,   194,   194,   195,   195,   195,
     196,   196,   197,   197,   198,   198,   199,   199,   200,   200,
     201,   201,   202,   202,   203,   203,   204,   204,   204,   204,
     204,   204,   204,   204,   204,   204,   204,   205,   205,   206,
     207,   207,   207,   207,   208,   209,   209,   210,   210,   211,
     212,   212,   212,   213,   213,   214,   214,   214,   214,   214,
     214,   215,   215,   215,   216,   217,   217,   217,   217,   217,
     218,   218,   218,   218,   218,   218,   218,   219,   219,   220,
     221,   221,   222,   222,   222,   223,   223,   223,   224,   224,
     225,   225,   226,   226,   226,   227,   227,   227,   227,   227,
     227,   227,   227,   227,   227,   227,   227,   227,   227,   227,
     227,   227,   227,   228,   228,   228,   228,   229,   229,   229,
     229,   229,   229,   229,   229,   229,   229,   229,   229,   229,
     230,   230,   230,   230,   230,   231,   231,   231,   231,   232,
     232,   233,   233,   233,   234,   234,   234,   235,   235,   235,
     236,   236,   237,   237,   238,   239,   239,   240,   240,   241,
     241,   241,   242,   242,   243,   244,   244,   245,   245,   245,
     245,   245,   245,   245,   246,   247,   246,   248,   248,   249,
     249,   250,   250,   250,   251,   251,   252,   253,   253,   254,
     254,   255,   256,   256,   257,   257,   258,   258,   259,   259,
     260,   260,   261,   261,   261,   262,   262,   263,   263,   264,
     264,   265,   265,   265,   265,   265,   266,   267,   267,   267,
     267,   267,   268,   269,   269,   269,   270,   271,   271,   271,
     271,   271,   271,   271,   271,   272,   272,   272,   273,   273,
     274,   275,   275,   276,   276,   277,   277,   278,   278,   279,
     279,   279,   279
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     0,     4,     0,     3,     4,     2,     2,     2,
       2,     2,     2,     2,     0,     2,     1,     1,     1,     5,
       1,     2,     2,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     3,     1,     4,     1,     3,
       2,     2,     1,     1,     1,     2,     2,     2,     1,     2,
       3,     2,     1,     1,     1,     2,     2,     2,     1,     1,
       1,     1,     1,     3,     3,     3,     1,     3,     3,     1,
       3,     3,     1,     3,     3,     3,     3,     1,     3,     3,
       1,     3,     1,     3,     1,     3,     1,     3,     1,     3,
       1,     3,     1,     5,     1,     3,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     3,     1,
       2,     2,     4,     1,     2,     1,     1,     2,     3,     3,
       2,     3,     3,     2,     2,     0,     2,     2,     2,     2,
       2,     1,     1,     1,     1,     1,     3,     4,     6,     5,
       1,     2,     3,     5,     4,     2,     2,     1,     2,     4,
       1,     3,     1,     3,     1,     1,     1,     1,     1,     4,
       1,     3,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     2,     3,     3,     4,     1,
       2,     1,     1,     1,     1,     1,     2,     1,     1,     1,
       5,     4,     1,     2,     3,     1,     3,     1,     2,     1,
       3,     4,     1,     3,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     2,     0,     4,     1,     1,     2,
       3,     1,     2,     2,     1,     2,     5,     3,     1,     1,
       4,     5,     2,     3,     3,     2,     1,     2,     2,     2,
       1,     2,     5,     7,     6,     1,     1,     1,     0,     2,
       3,     2,     2,     2,     3,     2,     2,     1,     1,     1,
       1,     1,     2,     1,     2,     2,     7,     1,     1,     1,
       1,     1,     1,     1,     2,     0,     1,     2,     1,     2,
       3,     2,     3,     2,     3,     2,     3,     2,     3,     1,
       1,     1,     1
};


enum { YYENOMEM = -2 };

#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYNOMEM         goto yyexhaustedlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
  do                                                              \
    if (yychar == YYEMPTY)                                        \
      {                                                           \
        yychar = (Token);                                         \
        yylval = (Value);                                         \
        YYPOPSTACK (yylen);                                       \
        yystate = *yyssp;                                         \
        goto yybackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        yyerror (&yylloc, state, YY_("syntax error: cannot back up")); \
        YYERROR;                                                  \
      }                                                           \
  while (0)

/* Backward compatibility with an undocumented macro.
   Use YYerror or YYUNDEF. */
#define YYERRCODE YYUNDEF

/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

#ifndef YYLLOC_DEFAULT
# define YYLLOC_DEFAULT(Current, Rhs, N)                                \
    do                                                                  \
      if (N)                                                            \
        {                                                               \
          (Current).first_line   = YYRHSLOC (Rhs, 1).first_line;        \
          (Current).first_column = YYRHSLOC (Rhs, 1).first_column;      \
          (Current).last_line    = YYRHSLOC (Rhs, N).last_line;         \
          (Current).last_column  = YYRHSLOC (Rhs, N).last_column;       \
        }                                                               \
      else                                                              \
        {                                                               \
          (Current).first_line   = (Current).last_line   =              \
            YYRHSLOC (Rhs, 0).last_line;                                \
          (Current).first_column = (Current).last_column =              \
            YYRHSLOC (Rhs, 0).last_column;                              \
        }                                                               \
    while (0)
#endif

#define YYRHSLOC(Rhs, K) ((Rhs)[K])


/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)


/* YYLOCATION_PRINT -- Print the location on the stream.
   This macro was not mandated originally: define only if we know
   we won't break user code: when these are the locations we know.  */

# ifndef YYLOCATION_PRINT

#  if defined YY_LOCATION_PRINT

   /* Temporary convenience wrapper in case some people defined the
      undocumented and private YY_LOCATION_PRINT macros.  */
#   define YYLOCATION_PRINT(File, Loc)  YY_LOCATION_PRINT(File, *(Loc))

#  elif defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL

/* Print *YYLOCP on YYO.  Private, do not rely on its existence. */

YY_ATTRIBUTE_UNUSED
static int
yy_location_print_ (FILE *yyo, YYLTYPE const * const yylocp)
{
  int res = 0;
  int end_col = 0 != yylocp->last_column ? yylocp->last_column - 1 : 0;
  if (0 <= yylocp->first_line)
    {
      res += YYFPRINTF (yyo, "%d", yylocp->first_line);
      if (0 <= yylocp->first_column)
        res += YYFPRINTF (yyo, ".%d", yylocp->first_column);
    }
  if (0 <= yylocp->last_line)
    {
      if (yylocp->first_line < yylocp->last_line)
        {
          res += YYFPRINTF (yyo, "-%d", yylocp->last_line);
          if (0 <= end_col)
            res += YYFPRINTF (yyo, ".%d", end_col);
        }
      else if (0 <= end_col && yylocp->first_column < end_col)
        res += YYFPRINTF (yyo, "-%d", end_col);
    }
  return res;
}

#   define YYLOCATION_PRINT  yy_location_print_

    /* Temporary convenience wrapper in case some people defined the
       undocumented and private YY_LOCATION_PRINT macros.  */
#   define YY_LOCATION_PRINT(File, Loc)  YYLOCATION_PRINT(File, &(Loc))

#  else

#   define YYLOCATION_PRINT(File, Loc) ((void) 0)
    /* Temporary convenience wrapper in case some people defined the
       undocumented and private YY_LOCATION_PRINT macros.  */
#   define YY_LOCATION_PRINT  YYLOCATION_PRINT

#  endif
# endif /* !defined YYLOCATION_PRINT */


# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Kind, Value, Location, state); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo,
                       yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep, YYLTYPE const * const yylocationp, struct _mesa_glsl_parse_state *state)
{
  FILE *yyoutput = yyo;
  YY_USE (yyoutput);
  YY_USE (yylocationp);
  YY_USE (state);
  if (!yyvaluep)
    return;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo,
                 yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep, YYLTYPE const * const yylocationp, struct _mesa_glsl_parse_state *state)
{
  YYFPRINTF (yyo, "%s %s (",
             yykind < YYNTOKENS ? "token" : "nterm", yysymbol_name (yykind));

  YYLOCATION_PRINT (yyo, yylocationp);
  YYFPRINTF (yyo, ": ");
  yy_symbol_value_print (yyo, yykind, yyvaluep, yylocationp, state);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp, YYLTYPE *yylsp,
                 int yyrule, struct _mesa_glsl_parse_state *state)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       YY_ACCESSING_SYMBOL (+yyssp[yyi + 1 - yynrhs]),
                       &yyvsp[(yyi + 1) - (yynrhs)],
                       &(yylsp[(yyi + 1) - (yynrhs)]), state);
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, yylsp, Rule, state); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args) ((void) 0)
# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif


/* Context of a parse error.  */
typedef struct
{
  yy_state_t *yyssp;
  yysymbol_kind_t yytoken;
  YYLTYPE *yylloc;
} yypcontext_t;

/* Put in YYARG at most YYARGN of the expected tokens given the
   current YYCTX, and return the number of tokens stored in YYARG.  If
   YYARG is null, return the number of expected tokens (guaranteed to
   be less than YYNTOKENS).  Return YYENOMEM on memory exhaustion.
   Return 0 if there are more than YYARGN expected tokens, yet fill
   YYARG up to YYARGN. */
static int
yypcontext_expected_tokens (const yypcontext_t *yyctx,
                            yysymbol_kind_t yyarg[], int yyargn)
{
  /* Actual size of YYARG. */
  int yycount = 0;
  int yyn = yypact[+*yyctx->yyssp];
  if (!yypact_value_is_default (yyn))
    {
      /* Start YYX at -YYN if negative to avoid negative indexes in
         YYCHECK.  In other words, skip the first -YYN actions for
         this state because they are default actions.  */
      int yyxbegin = yyn < 0 ? -yyn : 0;
      /* Stay within bounds of both yycheck and yytname.  */
      int yychecklim = YYLAST - yyn + 1;
      int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
      int yyx;
      for (yyx = yyxbegin; yyx < yyxend; ++yyx)
        if (yycheck[yyx + yyn] == yyx && yyx != YYSYMBOL_YYerror
            && !yytable_value_is_error (yytable[yyx + yyn]))
          {
            if (!yyarg)
              ++yycount;
            else if (yycount == yyargn)
              return 0;
            else
              yyarg[yycount++] = YY_CAST (yysymbol_kind_t, yyx);
          }
    }
  if (yyarg && yycount == 0 && 0 < yyargn)
    yyarg[0] = YYSYMBOL_YYEMPTY;
  return yycount;
}




#ifndef yystrlen
# if defined __GLIBC__ && defined _STRING_H
#  define yystrlen(S) (YY_CAST (YYPTRDIFF_T, strlen (S)))
# else
/* Return the length of YYSTR.  */
static YYPTRDIFF_T
yystrlen (const char *yystr)
{
  YYPTRDIFF_T yylen;
  for (yylen = 0; yystr[yylen]; yylen++)
    continue;
  return yylen;
}
# endif
#endif

#ifndef yystpcpy
# if defined __GLIBC__ && defined _STRING_H && defined _GNU_SOURCE
#  define yystpcpy stpcpy
# else
/* Copy YYSRC to YYDEST, returning the address of the terminating '\0' in
   YYDEST.  */
static char *
yystpcpy (char *yydest, const char *yysrc)
{
  char *yyd = yydest;
  const char *yys = yysrc;

  while ((*yyd++ = *yys++) != '\0')
    continue;

  return yyd - 1;
}
# endif
#endif

#ifndef yytnamerr
/* Copy to YYRES the contents of YYSTR after stripping away unnecessary
   quotes and backslashes, so that it's suitable for yyerror.  The
   heuristic is that double-quoting is unnecessary unless the string
   contains an apostrophe, a comma, or backslash (other than
   backslash-backslash).  YYSTR is taken from yytname.  If YYRES is
   null, do not copy; instead, return the length of what the result
   would have been.  */
static YYPTRDIFF_T
yytnamerr (char *yyres, const char *yystr)
{
  if (*yystr == '"')
    {
      YYPTRDIFF_T yyn = 0;
      char const *yyp = yystr;
      for (;;)
        switch (*++yyp)
          {
          case '\'':
          case ',':
            goto do_not_strip_quotes;

          case '\\':
            if (*++yyp != '\\')
              goto do_not_strip_quotes;
            else
              goto append;

          append:
          default:
            if (yyres)
              yyres[yyn] = *yyp;
            yyn++;
            break;

          case '"':
            if (yyres)
              yyres[yyn] = '\0';
            return yyn;
          }
    do_not_strip_quotes: ;
    }

  if (yyres)
    return yystpcpy (yyres, yystr) - yyres;
  else
    return yystrlen (yystr);
}
#endif


static int
yy_syntax_error_arguments (const yypcontext_t *yyctx,
                           yysymbol_kind_t yyarg[], int yyargn)
{
  /* Actual size of YYARG. */
  int yycount = 0;
  /* There are many possibilities here to consider:
     - If this state is a consistent state with a default action, then
       the only way this function was invoked is if the default action
       is an error action.  In that case, don't check for expected
       tokens because there are none.
     - The only way there can be no lookahead present (in yychar) is if
       this state is a consistent state with a default action.  Thus,
       detecting the absence of a lookahead is sufficient to determine
       that there is no unexpected or expected token to report.  In that
       case, just report a simple "syntax error".
     - Don't assume there isn't a lookahead just because this state is a
       consistent state with a default action.  There might have been a
       previous inconsistent state, consistent state with a non-default
       action, or user semantic action that manipulated yychar.
     - Of course, the expected token list depends on states to have
       correct lookahead information, and it depends on the parser not
       to perform extra reductions after fetching a lookahead from the
       scanner and before detecting a syntax error.  Thus, state merging
       (from LALR or IELR) and default reductions corrupt the expected
       token list.  However, the list is correct for canonical LR with
       one exception: it will still contain any token that will not be
       accepted due to an error action in a later state.
  */
  if (yyctx->yytoken != YYSYMBOL_YYEMPTY)
    {
      int yyn;
      if (yyarg)
        yyarg[yycount] = yyctx->yytoken;
      ++yycount;
      yyn = yypcontext_expected_tokens (yyctx,
                                        yyarg ? yyarg + 1 : yyarg, yyargn - 1);
      if (yyn == YYENOMEM)
        return YYENOMEM;
      else
        yycount += yyn;
    }
  return yycount;
}

/* Copy into *YYMSG, which is of size *YYMSG_ALLOC, an error message
   about the unexpected token YYTOKEN for the state stack whose top is
   YYSSP.

   Return 0 if *YYMSG was successfully written.  Return -1 if *YYMSG is
   not large enough to hold the message.  In that case, also set
   *YYMSG_ALLOC to the required number of bytes.  Return YYENOMEM if the
   required number of bytes is too large to store.  */
static int
yysyntax_error (YYPTRDIFF_T *yymsg_alloc, char **yymsg,
                const yypcontext_t *yyctx)
{
  enum { YYARGS_MAX = 5 };
  /* Internationalized format string. */
  const char *yyformat = YY_NULLPTR;
  /* Arguments of yyformat: reported tokens (one for the "unexpected",
     one per "expected"). */
  yysymbol_kind_t yyarg[YYARGS_MAX];
  /* Cumulated lengths of YYARG.  */
  YYPTRDIFF_T yysize = 0;

  /* Actual size of YYARG. */
  int yycount = yy_syntax_error_arguments (yyctx, yyarg, YYARGS_MAX);
  if (yycount == YYENOMEM)
    return YYENOMEM;

  switch (yycount)
    {
#define YYCASE_(N, S)                       \
      case N:                               \
        yyformat = S;                       \
        break
    default: /* Avoid compiler warnings. */
      YYCASE_(0, YY_("syntax error"));
      YYCASE_(1, YY_("syntax error, unexpected %s"));
      YYCASE_(2, YY_("syntax error, unexpected %s, expecting %s"));
      YYCASE_(3, YY_("syntax error, unexpected %s, expecting %s or %s"));
      YYCASE_(4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
      YYCASE_(5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
#undef YYCASE_
    }

  /* Compute error message size.  Don't count the "%s"s, but reserve
     room for the terminator.  */
  yysize = yystrlen (yyformat) - 2 * yycount + 1;
  {
    int yyi;
    for (yyi = 0; yyi < yycount; ++yyi)
      {
        YYPTRDIFF_T yysize1
          = yysize + yytnamerr (YY_NULLPTR, yytname[yyarg[yyi]]);
        if (yysize <= yysize1 && yysize1 <= YYSTACK_ALLOC_MAXIMUM)
          yysize = yysize1;
        else
          return YYENOMEM;
      }
  }

  if (*yymsg_alloc < yysize)
    {
      *yymsg_alloc = 2 * yysize;
      if (! (yysize <= *yymsg_alloc
             && *yymsg_alloc <= YYSTACK_ALLOC_MAXIMUM))
        *yymsg_alloc = YYSTACK_ALLOC_MAXIMUM;
      return -1;
    }

  /* Avoid sprintf, as that infringes on the user's name space.
     Don't have undefined behavior even if the translation
     produced a string with the wrong number of "%s"s.  */
  {
    char *yyp = *yymsg;
    int yyi = 0;
    while ((*yyp = *yyformat) != '\0')
      if (*yyp == '%' && yyformat[1] == 's' && yyi < yycount)
        {
          yyp += yytnamerr (yyp, yytname[yyarg[yyi++]]);
          yyformat += 2;
        }
      else
        {
          ++yyp;
          ++yyformat;
        }
  }
  return 0;
}


/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg,
            yysymbol_kind_t yykind, YYSTYPE *yyvaluep, YYLTYPE *yylocationp, struct _mesa_glsl_parse_state *state)
{
  YY_USE (yyvaluep);
  YY_USE (yylocationp);
  YY_USE (state);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}






/*----------.
| yyparse.  |
`----------*/

int
yyparse (struct _mesa_glsl_parse_state *state)
{
/* Lookahead token kind.  */
int yychar;


/* The semantic value of the lookahead symbol.  */
/* Default value used for initialization, for pacifying older GCCs
   or non-GCC compilers.  */
YY_INITIAL_VALUE (static YYSTYPE yyval_default;)
YYSTYPE yylval YY_INITIAL_VALUE (= yyval_default);

/* Location data for the lookahead symbol.  */
static YYLTYPE yyloc_default
# if defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL
  = { 1, 1, 1, 1 }
# endif
;
YYLTYPE yylloc = yyloc_default;

    /* Number of syntax errors so far.  */
    int yynerrs = 0;

    yy_state_fast_t yystate = 0;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus = 0;

    /* Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* Their size.  */
    YYPTRDIFF_T yystacksize = YYINITDEPTH;

    /* The state stack: array, bottom, top.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss = yyssa;
    yy_state_t *yyssp = yyss;

    /* The semantic value stack: array, bottom, top.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs = yyvsa;
    YYSTYPE *yyvsp = yyvs;

    /* The location stack: array, bottom, top.  */
    YYLTYPE yylsa[YYINITDEPTH];
    YYLTYPE *yyls = yylsa;
    YYLTYPE *yylsp = yyls;

  int yyn;
  /* The return value of yyparse.  */
  int yyresult;
  /* Lookahead symbol kind.  */
  yysymbol_kind_t yytoken = YYSYMBOL_YYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;
  YYLTYPE yyloc;

  /* The locations where the error started and ended.  */
  YYLTYPE yyerror_range[3];

  /* Buffer for error messages, and its allocated size.  */
  char yymsgbuf[128];
  char *yymsg = yymsgbuf;
  YYPTRDIFF_T yymsg_alloc = sizeof yymsgbuf;

#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N), yylsp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yychar = YYEMPTY; /* Cause a token to be read.  */


/* User initialization code.  */
#line 91 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
{
   yylloc.first_line = 1;
   yylloc.first_column = 1;
   yylloc.last_line = 1;
   yylloc.last_column = 1;
   yylloc.source = 0;
   yylloc.path = NULL;
}

#line 2513 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"

  yylsp[0] = yylloc;
  goto yysetstate;


/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END
  YY_STACK_PRINT (yyss, yyssp);

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    YYNOMEM;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;
        YYLTYPE *yyls1 = yyls;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
                    &yyls1, yysize * YYSIZEOF (*yylsp),
                    &yystacksize);
        yyss = yyss1;
        yyvs = yyvs1;
        yyls = yyls1;
      }
# else /* defined YYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        YYNOMEM;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          YYNOMEM;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
        YYSTACK_RELOCATE (yyls_alloc, yyls);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;
      yylsp = yyls + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */


  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;


/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token\n"));
      yychar = yylex (&yylval, &yylloc, state);
    }

  if (yychar <= YYEOF)
    {
      yychar = YYEOF;
      yytoken = YYSYMBOL_YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else if (yychar == YYerror)
    {
      /* The scanner already issued an error message, process directly
         to error recovery.  But do not keep the error token as
         lookahead, it is too special and may lead us to an endless
         loop in error recovery. */
      yychar = YYUNDEF;
      yytoken = YYSYMBOL_YYerror;
      yyerror_range[1] = yylloc;
      goto yyerrlab1;
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END
  *++yylsp = yylloc;

  /* Discard the shifted token.  */
  yychar = YYEMPTY;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];

  /* Default location. */
  YYLLOC_DEFAULT (yyloc, (yylsp - yylen), yylen);
  yyerror_range[1] = yyloc;
  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
  case 2: /* $@1: %empty  */
#line 301 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      _mesa_glsl_initialize_types(state);
   }
#line 2728 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 3: /* translation_unit: version_statement extension_statement_list $@1 external_declaration_list  */
#line 305 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      delete state->symbols;
      state->symbols = new(ralloc_parent(state)) glsl_symbol_table;
      if (state->es_shader) {
         if (state->stage == MESA_SHADER_FRAGMENT) {
            state->symbols->add_default_precision_qualifier("int", ast_precision_medium);
         } else {
            state->symbols->add_default_precision_qualifier("float", ast_precision_high);
            state->symbols->add_default_precision_qualifier("int", ast_precision_high);
         }
         state->symbols->add_default_precision_qualifier("sampler2D", ast_precision_low);
         state->symbols->add_default_precision_qualifier("samplerExternalOES", ast_precision_low);
         state->symbols->add_default_precision_qualifier("__samplerExternal2DY2YEXT", ast_precision_low);
         state->symbols->add_default_precision_qualifier("samplerCube", ast_precision_low);
         state->symbols->add_default_precision_qualifier("atomic_uint", ast_precision_high);
      }
      _mesa_glsl_initialize_types(state);
   }
#line 2751 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 5: /* version_statement: VERSION_TOK INTCONSTANT EOL  */
#line 328 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      state->process_version_directive(&(yylsp[-1]), (yyvsp[-1].n), NULL);
      if (state->error) {
         YYERROR;
      }
   }
#line 2762 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 6: /* version_statement: VERSION_TOK INTCONSTANT any_identifier EOL  */
#line 335 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      state->process_version_directive(&(yylsp[-2]), (yyvsp[-2].n), (yyvsp[-1].identifier));
      if (state->error) {
         YYERROR;
      }
   }
#line 2773 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 7: /* pragma_statement: PRAGMA_DEBUG_ON EOL  */
#line 344 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                       { (yyval.node) = NULL; }
#line 2779 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 8: /* pragma_statement: PRAGMA_DEBUG_OFF EOL  */
#line 345 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                          { (yyval.node) = NULL; }
#line 2785 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 9: /* pragma_statement: PRAGMA_OPTIMIZE_ON EOL  */
#line 346 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                            { (yyval.node) = NULL; }
#line 2791 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 10: /* pragma_statement: PRAGMA_OPTIMIZE_OFF EOL  */
#line 347 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                             { (yyval.node) = NULL; }
#line 2797 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 11: /* pragma_statement: PRAGMA_INVARIANT_ALL EOL  */
#line 349 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      /* Pragma invariant(all) cannot be used in a fragment shader.
       *
       * Page 27 of the GLSL 1.20 spec, Page 53 of the GLSL ES 3.00 spec:
       *
       *     "It is an error to use this pragma in a fragment shader."
       */
      if (state->is_version(120, 300) &&
          state->stage == MESA_SHADER_FRAGMENT) {
         _mesa_glsl_error(& (yylsp[-1]), state,
                          "pragma `invariant(all)' cannot be used "
                          "in a fragment shader.");
      } else if (!state->is_version(120, 100)) {
         _mesa_glsl_warning(& (yylsp[-1]), state,
                            "pragma `invariant(all)' not supported in %s "
                            "(GLSL ES 1.00 or GLSL 1.20 required)",
                            state->get_version_string());
      } else {
         state->all_invariant = true;
      }

      (yyval.node) = NULL;
   }
#line 2825 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 12: /* pragma_statement: PRAGMA_WARNING_ON EOL  */
#line 373 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *mem_ctx = state->linalloc;
      (yyval.node) = new(mem_ctx) ast_warnings_toggle(true);
   }
#line 2834 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 13: /* pragma_statement: PRAGMA_WARNING_OFF EOL  */
#line 378 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *mem_ctx = state->linalloc;
      (yyval.node) = new(mem_ctx) ast_warnings_toggle(false);
   }
#line 2843 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 19: /* extension_statement: EXTENSION any_identifier COLON any_identifier EOL  */
#line 397 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      if (!_mesa_glsl_process_extension((yyvsp[-3].identifier), & (yylsp[-3]), (yyvsp[-1].identifier), & (yylsp[-1]), state)) {
         YYERROR;
      }
   }
#line 2853 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 20: /* external_declaration_list: external_declaration  */
#line 406 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      /* FINISHME: The NULL test is required because pragmas are set to
       * FINISHME: NULL. (See production rule for external_declaration.)
       */
      if ((yyvsp[0].node) != NULL)
         state->translation_unit.push_tail(& (yyvsp[0].node)->link);
   }
#line 2865 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 21: /* external_declaration_list: external_declaration_list external_declaration  */
#line 414 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      /* FINISHME: The NULL test is required because pragmas are set to
       * FINISHME: NULL. (See production rule for external_declaration.)
       */
      if ((yyvsp[0].node) != NULL)
         state->translation_unit.push_tail(& (yyvsp[0].node)->link);
   }
#line 2877 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 22: /* external_declaration_list: external_declaration_list extension_statement  */
#line 421 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                                                   {
      if (!state->allow_extension_directive_midshader) {
         _mesa_glsl_error(& (yylsp[0]), state,
                          "#extension directive is not allowed "
                          "in the middle of a shader");
         YYERROR;
      }
   }
#line 2890 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 25: /* primary_expression: variable_identifier  */
#line 438 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_identifier, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[0]));
      (yyval.expression)->primary_expression.identifier = (yyvsp[0].identifier);
   }
#line 2901 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 26: /* primary_expression: INTCONSTANT  */
#line 445 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_int_constant, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[0]));
      (yyval.expression)->primary_expression.int_constant = (yyvsp[0].n);
   }
#line 2912 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 27: /* primary_expression: UINTCONSTANT  */
#line 452 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_uint_constant, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[0]));
      (yyval.expression)->primary_expression.uint_constant = (yyvsp[0].n);
   }
#line 2923 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 28: /* primary_expression: INT64CONSTANT  */
#line 459 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_int64_constant, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[0]));
      (yyval.expression)->primary_expression.int64_constant = (yyvsp[0].n64);
   }
#line 2934 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 29: /* primary_expression: UINT64CONSTANT  */
#line 466 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_uint64_constant, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[0]));
      (yyval.expression)->primary_expression.uint64_constant = (yyvsp[0].n64);
   }
#line 2945 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 30: /* primary_expression: FLOAT16CONSTANT  */
#line 473 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_float16_constant, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[0]));
      (yyval.expression)->primary_expression.float16_constant = (yyvsp[0].real);
   }
#line 2956 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 31: /* primary_expression: FLOATCONSTANT  */
#line 480 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_float_constant, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[0]));
      (yyval.expression)->primary_expression.float_constant = (yyvsp[0].real);
   }
#line 2967 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 32: /* primary_expression: DOUBLECONSTANT  */
#line 487 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_double_constant, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[0]));
      (yyval.expression)->primary_expression.double_constant = (yyvsp[0].dreal);
   }
#line 2978 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 33: /* primary_expression: BOOLCONSTANT  */
#line 494 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_bool_constant, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[0]));
      (yyval.expression)->primary_expression.bool_constant = (yyvsp[0].n);
   }
#line 2989 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 34: /* primary_expression: CSCSTANDARD  */
#line 501 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_csc_standard, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[0]));
      (yyval.expression)->primary_expression.csc_standard = (yyvsp[0].csc_standard);
   }
#line 3000 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 35: /* primary_expression: '(' expression ')'  */
#line 508 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.expression) = (yyvsp[-1].expression);
   }
#line 3008 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 37: /* postfix_expression: postfix_expression '[' integer_expression ']'  */
#line 516 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_array_index, (yyvsp[-3].expression), (yyvsp[-1].expression), NULL);
      (yyval.expression)->set_location_range((yylsp[-3]), (yylsp[0]));
   }
#line 3018 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 38: /* postfix_expression: function_call  */
#line 522 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.expression) = (yyvsp[0].expression);
   }
#line 3026 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 39: /* postfix_expression: postfix_expression DOT_TOK FIELD_SELECTION  */
#line 526 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_field_selection, (yyvsp[-2].expression), NULL, NULL);
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
      (yyval.expression)->primary_expression.identifier = (yyvsp[0].identifier);
   }
#line 3037 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 40: /* postfix_expression: postfix_expression INC_OP  */
#line 533 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_post_inc, (yyvsp[-1].expression), NULL, NULL);
      (yyval.expression)->set_location_range((yylsp[-1]), (yylsp[0]));
   }
#line 3047 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 41: /* postfix_expression: postfix_expression DEC_OP  */
#line 539 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_post_dec, (yyvsp[-1].expression), NULL, NULL);
      (yyval.expression)->set_location_range((yylsp[-1]), (yylsp[0]));
   }
#line 3057 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 49: /* function_call_header_with_parameters: function_call_header assignment_expression  */
#line 570 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.expression) = (yyvsp[-1].expression);
      (yyval.expression)->set_location((yylsp[-1]));
      (yyval.expression)->expressions.push_tail(& (yyvsp[0].expression)->link);
   }
#line 3067 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 50: /* function_call_header_with_parameters: function_call_header_with_parameters ',' assignment_expression  */
#line 576 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.expression) = (yyvsp[-2].expression);
      (yyval.expression)->set_location((yylsp[-2]));
      (yyval.expression)->expressions.push_tail(& (yyvsp[0].expression)->link);
   }
#line 3077 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 52: /* function_identifier: type_specifier  */
#line 592 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_function_expression((yyvsp[0].type_specifier));
      (yyval.expression)->set_location((yylsp[0]));
      }
#line 3087 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 53: /* function_identifier: postfix_expression  */
#line 598 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_function_expression((yyvsp[0].expression));
      (yyval.expression)->set_location((yylsp[0]));
      }
#line 3097 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 55: /* unary_expression: INC_OP unary_expression  */
#line 613 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_pre_inc, (yyvsp[0].expression), NULL, NULL);
      (yyval.expression)->set_location((yylsp[-1]));
   }
#line 3107 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 56: /* unary_expression: DEC_OP unary_expression  */
#line 619 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_pre_dec, (yyvsp[0].expression), NULL, NULL);
      (yyval.expression)->set_location((yylsp[-1]));
   }
#line 3117 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 57: /* unary_expression: unary_operator unary_expression  */
#line 625 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression((yyvsp[-1].n), (yyvsp[0].expression), NULL, NULL);
      (yyval.expression)->set_location_range((yylsp[-1]), (yylsp[0]));
   }
#line 3127 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 58: /* unary_operator: '+'  */
#line 634 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
         { (yyval.n) = ast_plus; }
#line 3133 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 59: /* unary_operator: '-'  */
#line 635 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
         { (yyval.n) = ast_neg; }
#line 3139 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 60: /* unary_operator: '!'  */
#line 636 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
         { (yyval.n) = ast_logic_not; }
#line 3145 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 61: /* unary_operator: '~'  */
#line 637 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
         { (yyval.n) = ast_bit_not; }
#line 3151 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 63: /* multiplicative_expression: multiplicative_expression '*' unary_expression  */
#line 643 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_mul, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3161 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 64: /* multiplicative_expression: multiplicative_expression '/' unary_expression  */
#line 649 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_div, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3171 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 65: /* multiplicative_expression: multiplicative_expression '%' unary_expression  */
#line 655 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_mod, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3181 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 67: /* additive_expression: additive_expression '+' multiplicative_expression  */
#line 665 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_add, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3191 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 68: /* additive_expression: additive_expression '-' multiplicative_expression  */
#line 671 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_sub, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3201 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 70: /* shift_expression: shift_expression LEFT_OP additive_expression  */
#line 681 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_lshift, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3211 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 71: /* shift_expression: shift_expression RIGHT_OP additive_expression  */
#line 687 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_rshift, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3221 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 73: /* relational_expression: relational_expression '<' shift_expression  */
#line 697 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_less, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3231 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 74: /* relational_expression: relational_expression '>' shift_expression  */
#line 703 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_greater, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3241 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 75: /* relational_expression: relational_expression LE_OP shift_expression  */
#line 709 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_lequal, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3251 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 76: /* relational_expression: relational_expression GE_OP shift_expression  */
#line 715 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_gequal, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3261 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 78: /* equality_expression: equality_expression EQ_OP relational_expression  */
#line 725 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_equal, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3271 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 79: /* equality_expression: equality_expression NE_OP relational_expression  */
#line 731 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_nequal, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3281 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 81: /* and_expression: and_expression '&' equality_expression  */
#line 741 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_bit_and, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3291 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 83: /* exclusive_or_expression: exclusive_or_expression '^' and_expression  */
#line 751 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_bit_xor, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3301 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 85: /* inclusive_or_expression: inclusive_or_expression '|' exclusive_or_expression  */
#line 761 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_bit_or, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3311 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 87: /* logical_and_expression: logical_and_expression AND_OP inclusive_or_expression  */
#line 771 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_logic_and, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3321 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 89: /* logical_xor_expression: logical_xor_expression XOR_OP logical_and_expression  */
#line 781 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_logic_xor, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3331 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 91: /* logical_or_expression: logical_or_expression OR_OP logical_xor_expression  */
#line 791 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_logic_or, (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3341 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 93: /* conditional_expression: logical_or_expression '?' expression ':' assignment_expression  */
#line 801 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_conditional, (yyvsp[-4].expression), (yyvsp[-2].expression), (yyvsp[0].expression));
      (yyval.expression)->set_location_range((yylsp[-4]), (yylsp[0]));
   }
#line 3351 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 95: /* assignment_expression: unary_expression assignment_operator assignment_expression  */
#line 811 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression((yyvsp[-1].n), (yyvsp[-2].expression), (yyvsp[0].expression), NULL);
      (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 3361 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 96: /* assignment_operator: '='  */
#line 819 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                      { (yyval.n) = ast_assign; }
#line 3367 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 97: /* assignment_operator: MUL_ASSIGN  */
#line 820 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                      { (yyval.n) = ast_mul_assign; }
#line 3373 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 98: /* assignment_operator: DIV_ASSIGN  */
#line 821 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                      { (yyval.n) = ast_div_assign; }
#line 3379 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 99: /* assignment_operator: MOD_ASSIGN  */
#line 822 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                      { (yyval.n) = ast_mod_assign; }
#line 3385 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 100: /* assignment_operator: ADD_ASSIGN  */
#line 823 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                      { (yyval.n) = ast_add_assign; }
#line 3391 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 101: /* assignment_operator: SUB_ASSIGN  */
#line 824 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                      { (yyval.n) = ast_sub_assign; }
#line 3397 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 102: /* assignment_operator: LEFT_ASSIGN  */
#line 825 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                      { (yyval.n) = ast_ls_assign; }
#line 3403 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 103: /* assignment_operator: RIGHT_ASSIGN  */
#line 826 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                      { (yyval.n) = ast_rs_assign; }
#line 3409 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 104: /* assignment_operator: AND_ASSIGN  */
#line 827 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                      { (yyval.n) = ast_and_assign; }
#line 3415 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 105: /* assignment_operator: XOR_ASSIGN  */
#line 828 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                      { (yyval.n) = ast_xor_assign; }
#line 3421 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 106: /* assignment_operator: OR_ASSIGN  */
#line 829 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                      { (yyval.n) = ast_or_assign; }
#line 3427 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 107: /* expression: assignment_expression  */
#line 834 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.expression) = (yyvsp[0].expression);
   }
#line 3435 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 108: /* expression: expression ',' assignment_expression  */
#line 838 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      if ((yyvsp[-2].expression)->oper != ast_sequence) {
         (yyval.expression) = new(ctx) ast_expression(ast_sequence, NULL, NULL, NULL);
         (yyval.expression)->set_location_range((yylsp[-2]), (yylsp[0]));
         (yyval.expression)->expressions.push_tail(& (yyvsp[-2].expression)->link);
      } else {
         (yyval.expression) = (yyvsp[-2].expression);
      }

      (yyval.expression)->expressions.push_tail(& (yyvsp[0].expression)->link);
   }
#line 3452 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 110: /* declaration: function_prototype ';'  */
#line 858 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      state->symbols->pop_scope();
      (yyval.node) = (yyvsp[-1].function);
   }
#line 3461 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 111: /* declaration: init_declarator_list ';'  */
#line 863 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.node) = (yyvsp[-1].declarator_list);
   }
#line 3469 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 112: /* declaration: PRECISION precision_qualifier type_specifier ';'  */
#line 867 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyvsp[-1].type_specifier)->default_precision = (yyvsp[-2].n);
      (yyval.node) = (yyvsp[-1].type_specifier);
   }
#line 3478 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 113: /* declaration: interface_block  */
#line 872 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      ast_interface_block *block = (ast_interface_block *) (yyvsp[0].node);
      if (block->layout.has_layout() || block->layout.has_memory()) {
         if (!block->default_layout.merge_qualifier(& (yylsp[0]), state, block->layout, false)) {
            YYERROR;
         }
      }
      block->layout = block->default_layout;
      if (!block->layout.push_to_global(& (yylsp[0]), state)) {
         YYERROR;
      }
      (yyval.node) = (yyvsp[0].node);
   }
#line 3496 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 117: /* function_header_with_parameters: function_header parameter_declaration  */
#line 898 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.function) = (yyvsp[-1].function);
      (yyval.function)->parameters.push_tail(& (yyvsp[0].parameter_declarator)->link);
   }
#line 3505 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 118: /* function_header_with_parameters: function_header_with_parameters ',' parameter_declaration  */
#line 903 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.function) = (yyvsp[-2].function);
      (yyval.function)->parameters.push_tail(& (yyvsp[0].parameter_declarator)->link);
   }
#line 3514 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 119: /* function_header: fully_specified_type variable_identifier '('  */
#line 911 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.function) = new(ctx) ast_function();
      (yyval.function)->set_location((yylsp[-1]));
      (yyval.function)->return_type = (yyvsp[-2].fully_specified_type);
      (yyval.function)->identifier = (yyvsp[-1].identifier);

      if ((yyvsp[-2].fully_specified_type)->qualifier.is_subroutine_decl()) {
         /* add type for IDENTIFIER search */
         state->symbols->add_type((yyvsp[-1].identifier), glsl_subroutine_type((yyvsp[-1].identifier)));
      } else
         state->symbols->add_function(new(ctx) ir_function((yyvsp[-1].identifier)));
      state->symbols->push_scope();
   }
#line 3533 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 120: /* parameter_declarator: type_specifier any_identifier  */
#line 929 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.parameter_declarator) = new(ctx) ast_parameter_declarator();
      (yyval.parameter_declarator)->set_location_range((yylsp[-1]), (yylsp[0]));
      (yyval.parameter_declarator)->type = new(ctx) ast_fully_specified_type();
      (yyval.parameter_declarator)->type->set_location((yylsp[-1]));
      (yyval.parameter_declarator)->type->specifier = (yyvsp[-1].type_specifier);
      (yyval.parameter_declarator)->identifier = (yyvsp[0].identifier);
      state->symbols->add_variable(new(ctx) ir_variable(NULL, (yyvsp[0].identifier), ir_var_auto));
   }
#line 3548 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 121: /* parameter_declarator: layout_qualifier type_specifier any_identifier  */
#line 940 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      _mesa_glsl_error(&(yylsp[-2]), state, "is is not allowed on function parameter");
      YYERROR;
   }
#line 3557 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 122: /* parameter_declarator: type_specifier any_identifier array_specifier  */
#line 945 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.parameter_declarator) = new(ctx) ast_parameter_declarator();
      (yyval.parameter_declarator)->set_location_range((yylsp[-2]), (yylsp[0]));
      (yyval.parameter_declarator)->type = new(ctx) ast_fully_specified_type();
      (yyval.parameter_declarator)->type->set_location((yylsp[-2]));
      (yyval.parameter_declarator)->type->specifier = (yyvsp[-2].type_specifier);
      (yyval.parameter_declarator)->identifier = (yyvsp[-1].identifier);
      (yyval.parameter_declarator)->array_specifier = (yyvsp[0].array_specifier);
      state->symbols->add_variable(new(ctx) ir_variable(NULL, (yyvsp[-1].identifier), ir_var_auto));
   }
#line 3573 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 123: /* parameter_declaration: parameter_qualifier parameter_declarator  */
#line 960 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.parameter_declarator) = (yyvsp[0].parameter_declarator);
      (yyval.parameter_declarator)->type->qualifier = (yyvsp[-1].type_qualifier);
      if (!(yyval.parameter_declarator)->type->qualifier.push_to_global(& (yylsp[-1]), state)) {
         YYERROR;
      }
   }
#line 3585 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 124: /* parameter_declaration: parameter_qualifier parameter_type_specifier  */
#line 968 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.parameter_declarator) = new(ctx) ast_parameter_declarator();
      (yyval.parameter_declarator)->set_location((yylsp[0]));
      (yyval.parameter_declarator)->type = new(ctx) ast_fully_specified_type();
      (yyval.parameter_declarator)->type->set_location_range((yylsp[-1]), (yylsp[0]));
      (yyval.parameter_declarator)->type->qualifier = (yyvsp[-1].type_qualifier);
      if (!(yyval.parameter_declarator)->type->qualifier.push_to_global(& (yylsp[-1]), state)) {
         YYERROR;
      }
      (yyval.parameter_declarator)->type->specifier = (yyvsp[0].type_specifier);
   }
#line 3602 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 125: /* parameter_qualifier: %empty  */
#line 984 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
   }
#line 3610 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 126: /* parameter_qualifier: CONST_TOK parameter_qualifier  */
#line 988 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      if ((yyvsp[0].type_qualifier).flags.q.constant)
         _mesa_glsl_error(&(yylsp[-1]), state, "duplicate const qualifier");

      (yyval.type_qualifier) = (yyvsp[0].type_qualifier);
      (yyval.type_qualifier).flags.q.constant = 1;
   }
#line 3622 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 127: /* parameter_qualifier: PRECISE parameter_qualifier  */
#line 996 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      if ((yyvsp[0].type_qualifier).flags.q.precise)
         _mesa_glsl_error(&(yylsp[-1]), state, "duplicate precise qualifier");

      (yyval.type_qualifier) = (yyvsp[0].type_qualifier);
      (yyval.type_qualifier).flags.q.precise = 1;
   }
#line 3634 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 128: /* parameter_qualifier: parameter_direction_qualifier parameter_qualifier  */
#line 1004 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      if (((yyvsp[-1].type_qualifier).flags.q.in || (yyvsp[-1].type_qualifier).flags.q.out) && ((yyvsp[0].type_qualifier).flags.q.in || (yyvsp[0].type_qualifier).flags.q.out))
         _mesa_glsl_error(&(yylsp[-1]), state, "duplicate in/out/inout qualifier");

      if (!state->has_420pack_or_es31() && (yyvsp[0].type_qualifier).flags.q.constant)
         _mesa_glsl_error(&(yylsp[-1]), state, "in/out/inout must come after const "
                                      "or precise");

      (yyval.type_qualifier) = (yyvsp[-1].type_qualifier);
      (yyval.type_qualifier).merge_qualifier(&(yylsp[-1]), state, (yyvsp[0].type_qualifier), false);
   }
#line 3650 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 129: /* parameter_qualifier: precision_qualifier parameter_qualifier  */
#line 1016 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      if ((yyvsp[0].type_qualifier).precision != ast_precision_none)
         _mesa_glsl_error(&(yylsp[-1]), state, "duplicate precision qualifier");

      if (!state->has_420pack_or_es31() &&
          (yyvsp[0].type_qualifier).flags.i != 0)
         _mesa_glsl_error(&(yylsp[-1]), state, "precision qualifiers must come last");

      (yyval.type_qualifier) = (yyvsp[0].type_qualifier);
      (yyval.type_qualifier).precision = (yyvsp[-1].n);
   }
#line 3666 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 130: /* parameter_qualifier: memory_qualifier parameter_qualifier  */
#line 1028 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.type_qualifier) = (yyvsp[-1].type_qualifier);
      (yyval.type_qualifier).merge_qualifier(&(yylsp[-1]), state, (yyvsp[0].type_qualifier), false);
   }
#line 3675 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 131: /* parameter_direction_qualifier: IN_TOK  */
#line 1035 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.in = 1;
   }
#line 3684 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 132: /* parameter_direction_qualifier: OUT_TOK  */
#line 1040 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.out = 1;
   }
#line 3693 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 133: /* parameter_direction_qualifier: INOUT_TOK  */
#line 1045 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.in = 1;
      (yyval.type_qualifier).flags.q.out = 1;
   }
#line 3703 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 136: /* init_declarator_list: init_declarator_list ',' any_identifier  */
#line 1059 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[0].identifier), NULL, NULL);
      decl->set_location((yylsp[0]));

      (yyval.declarator_list) = (yyvsp[-2].declarator_list);
      (yyval.declarator_list)->declarations.push_tail(&decl->link);
      state->symbols->add_variable(new(ctx) ir_variable(NULL, (yyvsp[0].identifier), ir_var_auto));
   }
#line 3717 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 137: /* init_declarator_list: init_declarator_list ',' any_identifier array_specifier  */
#line 1069 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[-1].identifier), (yyvsp[0].array_specifier), NULL);
      decl->set_location_range((yylsp[-1]), (yylsp[0]));

      (yyval.declarator_list) = (yyvsp[-3].declarator_list);
      (yyval.declarator_list)->declarations.push_tail(&decl->link);
      state->symbols->add_variable(new(ctx) ir_variable(NULL, (yyvsp[-1].identifier), ir_var_auto));
   }
#line 3731 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 138: /* init_declarator_list: init_declarator_list ',' any_identifier array_specifier '=' initializer  */
#line 1079 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[-3].identifier), (yyvsp[-2].array_specifier), (yyvsp[0].expression));
      decl->set_location_range((yylsp[-3]), (yylsp[-2]));

      (yyval.declarator_list) = (yyvsp[-5].declarator_list);
      (yyval.declarator_list)->declarations.push_tail(&decl->link);
      state->symbols->add_variable(new(ctx) ir_variable(NULL, (yyvsp[-3].identifier), ir_var_auto));
   }
#line 3745 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 139: /* init_declarator_list: init_declarator_list ',' any_identifier '=' initializer  */
#line 1089 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[-2].identifier), NULL, (yyvsp[0].expression));
      decl->set_location((yylsp[-2]));

      (yyval.declarator_list) = (yyvsp[-4].declarator_list);
      (yyval.declarator_list)->declarations.push_tail(&decl->link);
      state->symbols->add_variable(new(ctx) ir_variable(NULL, (yyvsp[-2].identifier), ir_var_auto));
   }
#line 3759 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 140: /* single_declaration: fully_specified_type  */
#line 1103 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      /* Empty declaration list is valid. */
      (yyval.declarator_list) = new(ctx) ast_declarator_list((yyvsp[0].fully_specified_type));
      (yyval.declarator_list)->set_location((yylsp[0]));
   }
#line 3770 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 141: /* single_declaration: fully_specified_type any_identifier  */
#line 1110 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[0].identifier), NULL, NULL);
      decl->set_location((yylsp[0]));

      (yyval.declarator_list) = new(ctx) ast_declarator_list((yyvsp[-1].fully_specified_type));
      (yyval.declarator_list)->set_location_range((yylsp[-1]), (yylsp[0]));
      (yyval.declarator_list)->declarations.push_tail(&decl->link);
      state->symbols->add_variable(new(ctx) ir_variable(NULL, (yyvsp[0].identifier), ir_var_auto));
   }
#line 3785 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 142: /* single_declaration: fully_specified_type any_identifier array_specifier  */
#line 1121 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[-1].identifier), (yyvsp[0].array_specifier), NULL);
      decl->set_location_range((yylsp[-1]), (yylsp[0]));

      (yyval.declarator_list) = new(ctx) ast_declarator_list((yyvsp[-2].fully_specified_type));
      (yyval.declarator_list)->set_location_range((yylsp[-2]), (yylsp[0]));
      (yyval.declarator_list)->declarations.push_tail(&decl->link);
      state->symbols->add_variable(new(ctx) ir_variable(NULL, (yyvsp[-1].identifier), ir_var_auto));
   }
#line 3800 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 143: /* single_declaration: fully_specified_type any_identifier array_specifier '=' initializer  */
#line 1132 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[-3].identifier), (yyvsp[-2].array_specifier), (yyvsp[0].expression));
      decl->set_location_range((yylsp[-3]), (yylsp[-2]));

      (yyval.declarator_list) = new(ctx) ast_declarator_list((yyvsp[-4].fully_specified_type));
      (yyval.declarator_list)->set_location_range((yylsp[-4]), (yylsp[-2]));
      (yyval.declarator_list)->declarations.push_tail(&decl->link);
      state->symbols->add_variable(new(ctx) ir_variable(NULL, (yyvsp[-3].identifier), ir_var_auto));
   }
#line 3815 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 144: /* single_declaration: fully_specified_type any_identifier '=' initializer  */
#line 1143 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[-2].identifier), NULL, (yyvsp[0].expression));
      decl->set_location((yylsp[-2]));

      (yyval.declarator_list) = new(ctx) ast_declarator_list((yyvsp[-3].fully_specified_type));
      (yyval.declarator_list)->set_location_range((yylsp[-3]), (yylsp[-2]));
      (yyval.declarator_list)->declarations.push_tail(&decl->link);
      state->symbols->add_variable(new(ctx) ir_variable(NULL, (yyvsp[-2].identifier), ir_var_auto));
   }
#line 3830 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 145: /* single_declaration: INVARIANT variable_identifier  */
#line 1154 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[0].identifier), NULL, NULL);
      decl->set_location((yylsp[0]));

      (yyval.declarator_list) = new(ctx) ast_declarator_list(NULL);
      (yyval.declarator_list)->set_location_range((yylsp[-1]), (yylsp[0]));
      (yyval.declarator_list)->invariant = true;

      (yyval.declarator_list)->declarations.push_tail(&decl->link);
   }
#line 3846 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 146: /* single_declaration: PRECISE variable_identifier  */
#line 1166 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[0].identifier), NULL, NULL);
      decl->set_location((yylsp[0]));

      (yyval.declarator_list) = new(ctx) ast_declarator_list(NULL);
      (yyval.declarator_list)->set_location_range((yylsp[-1]), (yylsp[0]));
      (yyval.declarator_list)->precise = true;

      (yyval.declarator_list)->declarations.push_tail(&decl->link);
   }
#line 3862 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 147: /* fully_specified_type: type_specifier  */
#line 1181 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.fully_specified_type) = new(ctx) ast_fully_specified_type();
      (yyval.fully_specified_type)->set_location((yylsp[0]));
      (yyval.fully_specified_type)->specifier = (yyvsp[0].type_specifier);
   }
#line 3873 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 148: /* fully_specified_type: type_qualifier type_specifier  */
#line 1188 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.fully_specified_type) = new(ctx) ast_fully_specified_type();
      (yyval.fully_specified_type)->set_location_range((yylsp[-1]), (yylsp[0]));
      (yyval.fully_specified_type)->qualifier = (yyvsp[-1].type_qualifier);
      if (!(yyval.fully_specified_type)->qualifier.push_to_global(& (yylsp[-1]), state)) {
         YYERROR;
      }
      (yyval.fully_specified_type)->specifier = (yyvsp[0].type_specifier);
      if ((yyval.fully_specified_type)->specifier->structure != NULL &&
          (yyval.fully_specified_type)->specifier->structure->is_declaration) {
            (yyval.fully_specified_type)->specifier->structure->layout = &(yyval.fully_specified_type)->qualifier;
      }
   }
#line 3892 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 149: /* layout_qualifier: LAYOUT_TOK '(' layout_qualifier_id_list ')'  */
#line 1206 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.type_qualifier) = (yyvsp[-1].type_qualifier);
   }
#line 3900 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 151: /* layout_qualifier_id_list: layout_qualifier_id_list ',' layout_qualifier_id  */
#line 1214 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.type_qualifier) = (yyvsp[-2].type_qualifier);
      if (!(yyval.type_qualifier).merge_qualifier(& (yylsp[0]), state, (yyvsp[0].type_qualifier), true)) {
         YYERROR;
      }
   }
#line 3911 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 152: /* layout_qualifier_id: any_identifier  */
#line 1224 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));

      /* Layout qualifiers for ARB_fragment_coord_conventions. */
      if (!(yyval.type_qualifier).flags.i && (state->ARB_fragment_coord_conventions_enable ||
                          state->is_version(150, 0))) {
         if (match_layout_qualifier((yyvsp[0].identifier), "origin_upper_left", state) == 0) {
            (yyval.type_qualifier).flags.q.origin_upper_left = 1;
         } else if (match_layout_qualifier((yyvsp[0].identifier), "pixel_center_integer",
                                           state) == 0) {
            (yyval.type_qualifier).flags.q.pixel_center_integer = 1;
         }

         if ((yyval.type_qualifier).flags.i && state->ARB_fragment_coord_conventions_warn) {
            _mesa_glsl_warning(& (yylsp[0]), state,
                               "GL_ARB_fragment_coord_conventions layout "
                               "identifier `%s' used", (yyvsp[0].identifier));
         }
      }

      /* Layout qualifiers for AMD/ARB_conservative_depth. */
      if (!(yyval.type_qualifier).flags.i &&
          (state->AMD_conservative_depth_enable ||
           state->ARB_conservative_depth_enable ||
           state->EXT_conservative_depth_enable ||
           state->is_version(420, 0))) {
         if (match_layout_qualifier((yyvsp[0].identifier), "depth_any", state) == 0) {
            (yyval.type_qualifier).flags.q.depth_type = 1;
            (yyval.type_qualifier).depth_type = ast_depth_any;
         } else if (match_layout_qualifier((yyvsp[0].identifier), "depth_greater", state) == 0) {
            (yyval.type_qualifier).flags.q.depth_type = 1;
            (yyval.type_qualifier).depth_type = ast_depth_greater;
         } else if (match_layout_qualifier((yyvsp[0].identifier), "depth_less", state) == 0) {
            (yyval.type_qualifier).flags.q.depth_type = 1;
            (yyval.type_qualifier).depth_type = ast_depth_less;
         } else if (match_layout_qualifier((yyvsp[0].identifier), "depth_unchanged",
                                           state) == 0) {
            (yyval.type_qualifier).flags.q.depth_type = 1;
            (yyval.type_qualifier).depth_type = ast_depth_unchanged;
         }

         if ((yyval.type_qualifier).flags.i && state->AMD_conservative_depth_warn) {
            _mesa_glsl_warning(& (yylsp[0]), state,
                               "GL_AMD_conservative_depth "
                               "layout qualifier `%s' is used", (yyvsp[0].identifier));
         }
         if ((yyval.type_qualifier).flags.i && state->ARB_conservative_depth_warn) {
            _mesa_glsl_warning(& (yylsp[0]), state,
                               "GL_ARB_conservative_depth "
                               "layout qualifier `%s' is used", (yyvsp[0].identifier));
         }
         if ((yyval.type_qualifier).flags.i && state->EXT_conservative_depth_warn) {
            _mesa_glsl_warning(& (yylsp[0]), state,
                               "GL_EXT_conservative_depth "
                               "layout qualifier `%s' is used", (yyvsp[0].identifier));
         }
      }

      /* See also interface_block_layout_qualifier. */
      if (!(yyval.type_qualifier).flags.i &&
          (state->has_uniform_buffer_objects() ||
           state->has_shader_storage_buffer_objects())) {
         if (match_layout_qualifier((yyvsp[0].identifier), "std140", state) == 0) {
            (yyval.type_qualifier).flags.q.std140 = 1;
         } else if (match_layout_qualifier((yyvsp[0].identifier), "shared", state) == 0) {
            (yyval.type_qualifier).flags.q.shared = 1;
         } else if (match_layout_qualifier((yyvsp[0].identifier), "std430", state) == 0) {
            (yyval.type_qualifier).flags.q.std430 = 1;
         } else if (match_layout_qualifier((yyvsp[0].identifier), "column_major", state) == 0) {
            (yyval.type_qualifier).flags.q.column_major = 1;
         /* "row_major" is a reserved word in GLSL 1.30+. Its token is parsed
          * below in the interface_block_layout_qualifier rule.
          *
          * It is not a reserved word in GLSL ES 3.00, so it's handled here as
          * an identifier.
          *
          * Also, this takes care of alternate capitalizations of
          * "row_major" (which is necessary because layout qualifiers
          * are case-insensitive in desktop GLSL).
          */
         } else if (match_layout_qualifier((yyvsp[0].identifier), "row_major", state) == 0) {
            (yyval.type_qualifier).flags.q.row_major = 1;
         /* "packed" is a reserved word in GLSL, and its token is
          * parsed below in the interface_block_layout_qualifier rule.
          * However, we must take care of alternate capitalizations of
          * "packed", because layout qualifiers are case-insensitive
          * in desktop GLSL.
          */
         } else if (match_layout_qualifier((yyvsp[0].identifier), "packed", state) == 0) {
           (yyval.type_qualifier).flags.q.packed = 1;
         }

         if ((yyval.type_qualifier).flags.i &&
             (state->ARB_uniform_buffer_object_warn ||
              state->ARB_shader_storage_buffer_object_warn)) {
            _mesa_glsl_warning(& (yylsp[0]), state,
                               "#version 140 / GL_ARB_uniform_buffer_object "
                               "/ GL_ARB_shader_storage_buffer_object"
                               "layout qualifier `%s' is used", (yyvsp[0].identifier));
         }
      }

      /* Layout qualifiers for GLSL 1.50 geometry shaders. */
      if (!(yyval.type_qualifier).flags.i) {
         static const struct {
            const char *s;
            GLenum e;
         } map[] = {
                 { "points", GL_POINTS },
                 { "lines", GL_LINES },
                 { "lines_adjacency", GL_LINES_ADJACENCY },
                 { "line_strip", GL_LINE_STRIP },
                 { "triangles", GL_TRIANGLES },
                 { "triangles_adjacency", GL_TRIANGLES_ADJACENCY },
                 { "triangle_strip", GL_TRIANGLE_STRIP },
         };
         for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
            if (match_layout_qualifier((yyvsp[0].identifier), map[i].s, state) == 0) {
               (yyval.type_qualifier).flags.q.prim_type = 1;
               (yyval.type_qualifier).prim_type = map[i].e;
               break;
            }
         }

         if ((yyval.type_qualifier).flags.i && !state->has_geometry_shader() &&
             !state->has_tessellation_shader() && !state->EXT_mesh_shader_enable) {
            _mesa_glsl_error(& (yylsp[0]), state, "#version 150 layout "
                             "qualifier `%s' used", (yyvsp[0].identifier));
         }
      }

      /* Layout qualifiers for ARB_shader_image_load_store. */
      if (state->has_shader_image_load_store() ||
          state->EXT_shader_pixel_local_storage_enable) {
         if (!(yyval.type_qualifier).flags.i) {
            static const struct {
               const char *name;
               enum pipe_format format;
               glsl_base_type base_type;
               /** Minimum desktop GLSL version required for the image
                * format.  Use 130 if already present in the original
                * ARB extension.
                */
               unsigned required_glsl;
               /** Minimum GLSL ES version required for the image format. */
               unsigned required_essl;
               /* NV_image_formats */
               bool nv_image_formats;
               bool ext_qualifiers;
               /* for __pixel_localEXT */
               bool pixel_local_qualifiers;
            } map[] = {
               { "rgba32f", PIPE_FORMAT_R32G32B32A32_FLOAT, GLSL_TYPE_FLOAT, 130, 310, false, false, false },
               { "rgba16f", PIPE_FORMAT_R16G16B16A16_FLOAT, GLSL_TYPE_FLOAT, 130, 310, false, false, false },
               { "rg32f", PIPE_FORMAT_R32G32_FLOAT, GLSL_TYPE_FLOAT, 130, 0, true, false, false },
               { "rg16f", PIPE_FORMAT_R16G16_FLOAT, GLSL_TYPE_FLOAT, 130, 0, true, false, true },
               { "r11f_g11f_b10f", PIPE_FORMAT_R11G11B10_FLOAT, GLSL_TYPE_FLOAT, 130, 0, true, false, true },
               { "r32f", PIPE_FORMAT_R32_FLOAT, GLSL_TYPE_FLOAT, 130, 310, false, false, true },
               { "r16f", PIPE_FORMAT_R16_FLOAT, GLSL_TYPE_FLOAT, 130, 0, true, false, false },
               { "rgba32ui", PIPE_FORMAT_R32G32B32A32_UINT, GLSL_TYPE_UINT, 130, 310, false, false, false },
               { "rgba16ui", PIPE_FORMAT_R16G16B16A16_UINT, GLSL_TYPE_UINT, 130, 310, false, false, false },
               { "rgb10_a2ui", PIPE_FORMAT_R10G10B10A2_UINT, GLSL_TYPE_UINT, 130, 0, true, false, true },
               { "rgba8ui", PIPE_FORMAT_R8G8B8A8_UINT, GLSL_TYPE_UINT, 130, 310, false, false, true },
               { "rg32ui", PIPE_FORMAT_R32G32_UINT, GLSL_TYPE_UINT, 130, 0, true, false, false },
               { "rg16ui", PIPE_FORMAT_R16G16_UINT, GLSL_TYPE_UINT, 130, 0, true, false, true },
               { "rg8ui", PIPE_FORMAT_R8G8_UINT, GLSL_TYPE_UINT, 130, 0, true, false, false },
               { "r32ui", PIPE_FORMAT_R32_UINT, GLSL_TYPE_UINT, 130, 310, false, false, true },
               { "r16ui", PIPE_FORMAT_R16_UINT, GLSL_TYPE_UINT, 130, 0, true, false, false },
               { "r8ui", PIPE_FORMAT_R8_UINT, GLSL_TYPE_UINT, 130, 0, true, false, false },
               { "rgba32i", PIPE_FORMAT_R32G32B32A32_SINT, GLSL_TYPE_INT, 130, 310, false, false, false },
               { "rgba16i", PIPE_FORMAT_R16G16B16A16_SINT, GLSL_TYPE_INT, 130, 310, false, false, false },
               { "rgba8i", PIPE_FORMAT_R8G8B8A8_SINT, GLSL_TYPE_INT, 130, 310, false, false, true },
               { "rg32i", PIPE_FORMAT_R32G32_SINT, GLSL_TYPE_INT, 130, 0, true, false, false },
               { "rg16i", PIPE_FORMAT_R16G16_SINT, GLSL_TYPE_INT, 130, 0, true, false, true },
               { "rg8i", PIPE_FORMAT_R8G8_SINT, GLSL_TYPE_INT, 130, 0, true, false, false },
               { "r32i", PIPE_FORMAT_R32_SINT, GLSL_TYPE_INT, 130, 310, false, false, true },
               { "r16i", PIPE_FORMAT_R16_SINT, GLSL_TYPE_INT, 130, 0, true, false, false },
               { "r8i", PIPE_FORMAT_R8_SINT, GLSL_TYPE_INT, 130, 0, true, false, false },
               { "rgba16", PIPE_FORMAT_R16G16B16A16_UNORM, GLSL_TYPE_FLOAT, 130, 0, true, false, false },
               { "rgb10_a2", PIPE_FORMAT_R10G10B10A2_UNORM, GLSL_TYPE_FLOAT, 130, 0, true, false, true },
               { "rgba8", PIPE_FORMAT_R8G8B8A8_UNORM, GLSL_TYPE_FLOAT, 130, 310, false, false, true },
               { "rg16", PIPE_FORMAT_R16G16_UNORM, GLSL_TYPE_FLOAT, 130, 0, true, false, true },
               { "rg8", PIPE_FORMAT_R8G8_UNORM, GLSL_TYPE_FLOAT, 130, 0, true, false, false },
               { "r16", PIPE_FORMAT_R16_UNORM, GLSL_TYPE_FLOAT, 130, 0, true, false, false },
               { "r8", PIPE_FORMAT_R8_UNORM, GLSL_TYPE_FLOAT, 130, 0, true, false, false },
               { "rgba16_snorm", PIPE_FORMAT_R16G16B16A16_SNORM, GLSL_TYPE_FLOAT, 130, 0, true, false, false },
               { "rgba8_snorm", PIPE_FORMAT_R8G8B8A8_SNORM, GLSL_TYPE_FLOAT, 130, 310, false, false, false },
               { "rg16_snorm", PIPE_FORMAT_R16G16_SNORM, GLSL_TYPE_FLOAT, 130, 0, true, false, false },
               { "rg8_snorm", PIPE_FORMAT_R8G8_SNORM, GLSL_TYPE_FLOAT, 130, 0, true, false, false },
               { "r16_snorm", PIPE_FORMAT_R16_SNORM, GLSL_TYPE_FLOAT, 130, 0, true, false, false },
               { "r8_snorm", PIPE_FORMAT_R8_SNORM, GLSL_TYPE_FLOAT, 130, 0, true, false, false },

               /* From GL_EXT_shader_image_load_store: */
               /* base_type is incorrect but it'll be patched later when we know
                * the variable type. See ast_to_hir.cpp */
               { "size1x8", PIPE_FORMAT_R8_SINT, GLSL_TYPE_VOID, 130, 0, false, true, false },
               { "size1x16", PIPE_FORMAT_R16_SINT, GLSL_TYPE_VOID, 130, 0, false, true, false },
               { "size1x32", PIPE_FORMAT_R32_SINT, GLSL_TYPE_VOID, 130, 0, false, true, false },
               { "size2x32", PIPE_FORMAT_R32G32_SINT, GLSL_TYPE_VOID, 130, 0, false, true, false },
               { "size4x32", PIPE_FORMAT_R32G32B32A32_SINT, GLSL_TYPE_VOID, 130, 0, false, true, false },
            };

            for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
               if (state->EXT_shader_pixel_local_storage_enable &&
                   map[i].pixel_local_qualifiers &&
                   match_layout_qualifier((yyvsp[0].identifier), map[i].name, state) == 0) {
                  (yyval.type_qualifier).flags.q.explicit_image_format = 1;
                  (yyval.type_qualifier).image_format = map[i].format;
                  (yyval.type_qualifier).image_base_type = map[i].base_type;
                  break;
               }
               if ((state->is_version(map[i].required_glsl,
                                      map[i].required_essl) ||
                    (state->NV_image_formats_enable &&
                     map[i].nv_image_formats)) &&
                   match_layout_qualifier((yyvsp[0].identifier), map[i].name, state) == 0) {
                  /* Skip ARB_shader_image_load_store qualifiers if not enabled */
                  if (!map[i].ext_qualifiers && !(state->ARB_shader_image_load_store_enable ||
                                                  state->is_version(420, 310))) {
                     continue;
                  }
                  /* Skip EXT_shader_image_load_store qualifiers if not enabled */
                  if (map[i].ext_qualifiers && !state->EXT_shader_image_load_store_enable) {
                     continue;
                  }
                  (yyval.type_qualifier).flags.q.explicit_image_format = 1;
                  (yyval.type_qualifier).image_format = map[i].format;
                  (yyval.type_qualifier).image_base_type = map[i].base_type;
                  break;
               }
            }
         }
      }

      if (!(yyval.type_qualifier).flags.i) {
         if (match_layout_qualifier((yyvsp[0].identifier), "early_fragment_tests", state) == 0) {
            /* From section 4.4.1.3 of the GLSL 4.50 specification
             * (Fragment Shader Inputs):
             *
             *  "Fragment shaders also allow the following layout
             *   qualifier on in only (not with variable declarations)
             *     layout-qualifier-id
             *        early_fragment_tests
             *   [...]"
             */
            if (state->stage != MESA_SHADER_FRAGMENT) {
               _mesa_glsl_error(& (yylsp[0]), state,
                                "early_fragment_tests layout qualifier only "
                                "valid in fragment shaders");
            }

            (yyval.type_qualifier).flags.q.early_fragment_tests = 1;
         }

         if (match_layout_qualifier((yyvsp[0].identifier), "inner_coverage", state) == 0) {
            if (state->stage != MESA_SHADER_FRAGMENT) {
               _mesa_glsl_error(& (yylsp[0]), state,
                                "inner_coverage layout qualifier only "
                                "valid in fragment shaders");
            }

            if (state->INTEL_conservative_rasterization_enable) {
               (yyval.type_qualifier).flags.q.inner_coverage = 1;
            } else {
               _mesa_glsl_error(& (yylsp[0]), state,
                                "inner_coverage layout qualifier present, "
                                "but the INTEL_conservative_rasterization extension "
                                "is not enabled.");
            }
         }

         if (match_layout_qualifier((yyvsp[0].identifier), "post_depth_coverage", state) == 0) {
            if (state->stage != MESA_SHADER_FRAGMENT) {
               _mesa_glsl_error(& (yylsp[0]), state,
                                "post_depth_coverage layout qualifier only "
                                "valid in fragment shaders");
            }

            if (state->ARB_post_depth_coverage_enable ||
                state->INTEL_conservative_rasterization_enable) {
               (yyval.type_qualifier).flags.q.post_depth_coverage = 1;
            } else {
               _mesa_glsl_error(& (yylsp[0]), state,
                                "post_depth_coverage layout qualifier present, "
                                "but the GL_ARB_post_depth_coverage extension "
                                "is not enabled.");
            }
         }

         if ((yyval.type_qualifier).flags.q.post_depth_coverage && (yyval.type_qualifier).flags.q.inner_coverage) {
            _mesa_glsl_error(& (yylsp[0]), state,
                             "post_depth_coverage & inner_coverage layout qualifiers "
                             "are mutually exclusive");
         }
      }

      const bool pixel_interlock_ordered = match_layout_qualifier((yyvsp[0].identifier),
         "pixel_interlock_ordered", state) == 0;
      const bool pixel_interlock_unordered = match_layout_qualifier((yyvsp[0].identifier),
         "pixel_interlock_unordered", state) == 0;
      const bool sample_interlock_ordered = match_layout_qualifier((yyvsp[0].identifier),
         "sample_interlock_ordered", state) == 0;
      const bool sample_interlock_unordered = match_layout_qualifier((yyvsp[0].identifier),
         "sample_interlock_unordered", state) == 0;

      if (pixel_interlock_ordered + pixel_interlock_unordered +
          sample_interlock_ordered + sample_interlock_unordered > 0 &&
          state->stage != MESA_SHADER_FRAGMENT) {
         _mesa_glsl_error(& (yylsp[0]), state, "interlock layout qualifiers: "
                          "pixel_interlock_ordered, pixel_interlock_unordered, "
                          "sample_interlock_ordered and sample_interlock_unordered, "
                          "only valid in fragment shader input layout declaration.");
      } else if (pixel_interlock_ordered + pixel_interlock_unordered +
                 sample_interlock_ordered + sample_interlock_unordered > 0 &&
                 !state->ARB_fragment_shader_interlock_enable &&
                 !state->NV_fragment_shader_interlock_enable) {
         _mesa_glsl_error(& (yylsp[0]), state,
                          "interlock layout qualifier present, but the "
                          "GL_ARB_fragment_shader_interlock or "
                          "GL_NV_fragment_shader_interlock extension is not "
                          "enabled.");
      } else {
         (yyval.type_qualifier).flags.q.pixel_interlock_ordered = pixel_interlock_ordered;
         (yyval.type_qualifier).flags.q.pixel_interlock_unordered = pixel_interlock_unordered;
         (yyval.type_qualifier).flags.q.sample_interlock_ordered = sample_interlock_ordered;
         (yyval.type_qualifier).flags.q.sample_interlock_unordered = sample_interlock_unordered;
      }

      /* Layout qualifiers for tessellation evaluation shaders. */
      if (!(yyval.type_qualifier).flags.i) {
         static const struct {
            const char *s;
            GLenum e;
         } map[] = {
                 /* triangles already parsed by gs-specific code */
                 { "quads", GL_QUADS },
                 { "isolines", GL_ISOLINES },
         };
         for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
            if (match_layout_qualifier((yyvsp[0].identifier), map[i].s, state) == 0) {
               (yyval.type_qualifier).flags.q.prim_type = 1;
               (yyval.type_qualifier).prim_type = map[i].e;
               break;
            }
         }

         if ((yyval.type_qualifier).flags.i && !state->has_tessellation_shader()) {
            _mesa_glsl_error(& (yylsp[0]), state,
                             "primitive mode qualifier `%s' requires "
                             "GLSL 4.00 or ARB_tessellation_shader", (yyvsp[0].identifier));
         }
      }
      if (!(yyval.type_qualifier).flags.i) {
         static const struct {
            const char *s;
            enum gl_tess_spacing e;
         } map[] = {
                 { "equal_spacing", TESS_SPACING_EQUAL },
                 { "fractional_odd_spacing", TESS_SPACING_FRACTIONAL_ODD },
                 { "fractional_even_spacing", TESS_SPACING_FRACTIONAL_EVEN },
         };
         for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
            if (match_layout_qualifier((yyvsp[0].identifier), map[i].s, state) == 0) {
               (yyval.type_qualifier).flags.q.vertex_spacing = 1;
               (yyval.type_qualifier).vertex_spacing = map[i].e;
               break;
            }
         }

         if ((yyval.type_qualifier).flags.i && !state->has_tessellation_shader()) {
            _mesa_glsl_error(& (yylsp[0]), state,
                             "vertex spacing qualifier `%s' requires "
                             "GLSL 4.00 or ARB_tessellation_shader", (yyvsp[0].identifier));
         }
      }
      if (!(yyval.type_qualifier).flags.i) {
         if (match_layout_qualifier((yyvsp[0].identifier), "cw", state) == 0) {
            (yyval.type_qualifier).flags.q.ordering = 1;
            (yyval.type_qualifier).ordering = GL_CW;
         } else if (match_layout_qualifier((yyvsp[0].identifier), "ccw", state) == 0) {
            (yyval.type_qualifier).flags.q.ordering = 1;
            (yyval.type_qualifier).ordering = GL_CCW;
         }

         if ((yyval.type_qualifier).flags.i && !state->has_tessellation_shader()) {
            _mesa_glsl_error(& (yylsp[0]), state,
                             "ordering qualifier `%s' requires "
                             "GLSL 4.00 or ARB_tessellation_shader", (yyvsp[0].identifier));
         }
      }
      if (!(yyval.type_qualifier).flags.i) {
         if (match_layout_qualifier((yyvsp[0].identifier), "point_mode", state) == 0) {
            (yyval.type_qualifier).flags.q.point_mode = 1;
            (yyval.type_qualifier).point_mode = true;
         }

         if ((yyval.type_qualifier).flags.i && !state->has_tessellation_shader()) {
            _mesa_glsl_error(& (yylsp[0]), state,
                             "qualifier `point_mode' requires "
                             "GLSL 4.00 or ARB_tessellation_shader");
         }
      }

      if (!(yyval.type_qualifier).flags.i) {
         static const struct {
            const char *s;
            uint32_t mask;
         } map[] = {
                 { "blend_support_multiply",       BITFIELD_BIT(PIPE_ADVANCED_BLEND_MULTIPLY) },
                 { "blend_support_screen",         BITFIELD_BIT(PIPE_ADVANCED_BLEND_SCREEN) },
                 { "blend_support_overlay",        BITFIELD_BIT(PIPE_ADVANCED_BLEND_OVERLAY) },
                 { "blend_support_darken",         BITFIELD_BIT(PIPE_ADVANCED_BLEND_DARKEN) },
                 { "blend_support_lighten",        BITFIELD_BIT(PIPE_ADVANCED_BLEND_LIGHTEN) },
                 { "blend_support_colordodge",     BITFIELD_BIT(PIPE_ADVANCED_BLEND_COLORDODGE) },
                 { "blend_support_colorburn",      BITFIELD_BIT(PIPE_ADVANCED_BLEND_COLORBURN) },
                 { "blend_support_hardlight",      BITFIELD_BIT(PIPE_ADVANCED_BLEND_HARDLIGHT) },
                 { "blend_support_softlight",      BITFIELD_BIT(PIPE_ADVANCED_BLEND_SOFTLIGHT) },
                 { "blend_support_difference",     BITFIELD_BIT(PIPE_ADVANCED_BLEND_DIFFERENCE) },
                 { "blend_support_exclusion",      BITFIELD_BIT(PIPE_ADVANCED_BLEND_EXCLUSION) },
                 { "blend_support_hsl_hue",        BITFIELD_BIT(PIPE_ADVANCED_BLEND_HSL_HUE) },
                 { "blend_support_hsl_saturation", BITFIELD_BIT(PIPE_ADVANCED_BLEND_HSL_SATURATION) },
                 { "blend_support_hsl_color",      BITFIELD_BIT(PIPE_ADVANCED_BLEND_HSL_COLOR) },
                 { "blend_support_hsl_luminosity", BITFIELD_BIT(PIPE_ADVANCED_BLEND_HSL_LUMINOSITY) },
                 { "blend_support_all_equations",  PIPE_ADVANCED_BLEND_KHR_MODES_MASK },
         };
         for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
            if (match_layout_qualifier((yyvsp[0].identifier), map[i].s, state) == 0) {
               (yyval.type_qualifier).flags.q.blend_support = 1;
               state->fs_blend_support |= map[i].mask;
               break;
            }
         }

         if ((yyval.type_qualifier).flags.i &&
             !state->KHR_blend_equation_advanced_enable &&
             !state->is_version(0, 320)) {
            _mesa_glsl_error(& (yylsp[0]), state,
                             "advanced blending layout qualifiers require "
                             "ESSL 3.20 or KHR_blend_equation_advanced");
         }

         if ((yyval.type_qualifier).flags.i && state->stage != MESA_SHADER_FRAGMENT) {
            _mesa_glsl_error(& (yylsp[0]), state,
                             "advanced blending layout qualifiers only "
                             "valid in fragment shaders");
         }
      }

      /* Layout qualifiers for ARB_compute_variable_group_size. */
      if (!(yyval.type_qualifier).flags.i) {
         if (match_layout_qualifier((yyvsp[0].identifier), "local_size_variable", state) == 0) {
            (yyval.type_qualifier).flags.q.local_size_variable = 1;
         }

         if ((yyval.type_qualifier).flags.i && !state->ARB_compute_variable_group_size_enable) {
            _mesa_glsl_error(& (yylsp[0]), state,
                             "qualifier `local_size_variable` requires "
                             "ARB_compute_variable_group_size");
         }
      }

      /* Layout qualifiers for ARB_bindless_texture. */
      if (!(yyval.type_qualifier).flags.i) {
         if (match_layout_qualifier((yyvsp[0].identifier), "bindless_sampler", state) == 0)
            (yyval.type_qualifier).flags.q.bindless_sampler = 1;
         if (match_layout_qualifier((yyvsp[0].identifier), "bound_sampler", state) == 0)
            (yyval.type_qualifier).flags.q.bound_sampler = 1;

         if (state->has_shader_image_load_store()) {
            if (match_layout_qualifier((yyvsp[0].identifier), "bindless_image", state) == 0)
               (yyval.type_qualifier).flags.q.bindless_image = 1;
            if (match_layout_qualifier((yyvsp[0].identifier), "bound_image", state) == 0)
               (yyval.type_qualifier).flags.q.bound_image = 1;
         }

         if ((yyval.type_qualifier).flags.i && !state->has_bindless()) {
            _mesa_glsl_error(& (yylsp[0]), state,
                             "qualifier `%s` requires "
                             "ARB_bindless_texture", (yyvsp[0].identifier));
         }
      }

      if (!(yyval.type_qualifier).flags.i &&
          state->EXT_shader_framebuffer_fetch_non_coherent_enable) {
         if (match_layout_qualifier((yyvsp[0].identifier), "noncoherent", state) == 0)
            (yyval.type_qualifier).flags.q.non_coherent = 1;
      }

      // Layout qualifiers for NV_compute_shader_derivatives.
      if (!(yyval.type_qualifier).flags.i) {
         if (match_layout_qualifier((yyvsp[0].identifier), "derivative_group_quadsNV", state) == 0) {
            (yyval.type_qualifier).flags.q.derivative_group = 1;
            (yyval.type_qualifier).derivative_group = DERIVATIVE_GROUP_QUADS;
         } else if (match_layout_qualifier((yyvsp[0].identifier), "derivative_group_linearNV", state) == 0) {
            (yyval.type_qualifier).flags.q.derivative_group = 1;
            (yyval.type_qualifier).derivative_group = DERIVATIVE_GROUP_LINEAR;
         }

         if ((yyval.type_qualifier).flags.i) {
            if (!state->has_compute_shader()) {
               _mesa_glsl_error(& (yylsp[0]), state,
                                "qualifier `%s' requires "
                                "a compute shader", (yyvsp[0].identifier));
            }

            if (!state->NV_compute_shader_derivatives_enable) {
               _mesa_glsl_error(& (yylsp[0]), state,
                                "qualifier `%s' requires "
                                "NV_compute_shader_derivatives", (yyvsp[0].identifier));
            }

            if (state->NV_compute_shader_derivatives_warn) {
               _mesa_glsl_warning(& (yylsp[0]), state,
                                  "NV_compute_shader_derivatives layout "
                                  "qualifier `%s' used", (yyvsp[0].identifier));
            }
         }
      }

      /* Layout qualifier for NV_viewport_array2. */
      if (!(yyval.type_qualifier).flags.i && state->stage != MESA_SHADER_FRAGMENT) {
         if (match_layout_qualifier((yyvsp[0].identifier), "viewport_relative", state) == 0) {
            (yyval.type_qualifier).flags.q.viewport_relative = 1;
         }

         if ((yyval.type_qualifier).flags.i && !state->NV_viewport_array2_enable) {
            _mesa_glsl_error(& (yylsp[0]), state,
                             "qualifier `%s' requires "
                             "GL_NV_viewport_array2", (yyvsp[0].identifier));
         }

         if ((yyval.type_qualifier).flags.i && state->NV_viewport_array2_warn) {
            _mesa_glsl_warning(& (yylsp[0]), state,
                               "GL_NV_viewport_array2 layout "
                               "identifier `%s' used", (yyvsp[0].identifier));
         }
      }

      /* Layout qualifier for EXT_YUV_target. */
      if (match_layout_qualifier((yyvsp[0].identifier), "yuv", state) == 0) {
         if (state->stage != MESA_SHADER_FRAGMENT) {
            _mesa_glsl_error(& (yylsp[0]), state,
                              "yuv layout qualifier only valid in fragment "
                              "shaders");
         }

         if (state->EXT_YUV_target_enable) {
            (yyval.type_qualifier).flags.q.yuv = 1;
         } else {
            _mesa_glsl_error(& (yylsp[0]), state,
                              "yuv layout qualifier present, but the "
                              "EXT_YUV_target_enable extension is not "
                              "enabled.");
         }
      }

      if (!(yyval.type_qualifier).flags.i) {
         _mesa_glsl_error(& (yylsp[0]), state, "unrecognized layout identifier "
                          "`%s'", (yyvsp[0].identifier));
         YYERROR;
      }
   }
#line 4478 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 153: /* layout_qualifier_id: any_identifier '=' constant_expression  */
#line 1787 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      linear_ctx *ctx = state->linalloc;

      if ((yyvsp[0].expression)->oper != ast_int_constant &&
          (yyvsp[0].expression)->oper != ast_uint_constant &&
          !state->has_enhanced_layouts()) {
         _mesa_glsl_error(& (yylsp[-2]), state,
                          "compile-time constant expressions require "
                          "GLSL 4.40 or ARB_enhanced_layouts");
      }

      if (match_layout_qualifier("align", (yyvsp[-2].identifier), state) == 0) {
         if (!state->has_enhanced_layouts()) {
            _mesa_glsl_error(& (yylsp[-2]), state,
                             "align qualifier requires "
                             "GLSL 4.40 or ARB_enhanced_layouts");
         } else {
            (yyval.type_qualifier).flags.q.explicit_align = 1;
            (yyval.type_qualifier).align = (yyvsp[0].expression);
         }
      }

      if (match_layout_qualifier("location", (yyvsp[-2].identifier), state) == 0) {
         (yyval.type_qualifier).flags.q.explicit_location = 1;

         if ((yyval.type_qualifier).flags.q.attribute == 1 &&
             state->ARB_explicit_attrib_location_warn) {
            _mesa_glsl_warning(& (yylsp[-2]), state,
                               "GL_ARB_explicit_attrib_location layout "
                               "identifier `%s' used", (yyvsp[-2].identifier));
         }
         (yyval.type_qualifier).location = (yyvsp[0].expression);
      }

      if (match_layout_qualifier("num_views", (yyvsp[-2].identifier), state) == 0) {
         (yyval.type_qualifier).flags.q.explicit_numviews = 1;
         (yyval.type_qualifier).num_views = (yyvsp[0].expression);
      }

      if (match_layout_qualifier("component", (yyvsp[-2].identifier), state) == 0) {
         if (!state->has_enhanced_layouts()) {
            _mesa_glsl_error(& (yylsp[-2]), state,
                             "component qualifier requires "
                             "GLSL 4.40 or ARB_enhanced_layouts");
         } else {
            (yyval.type_qualifier).flags.q.explicit_component = 1;
            (yyval.type_qualifier).component = (yyvsp[0].expression);
         }
      }

      if (match_layout_qualifier("index", (yyvsp[-2].identifier), state) == 0) {
         if (state->es_shader && !state->EXT_blend_func_extended_enable) {
            _mesa_glsl_error(& (yylsp[0]), state, "index layout qualifier requires EXT_blend_func_extended");
            YYERROR;
         }

         (yyval.type_qualifier).flags.q.explicit_index = 1;
         (yyval.type_qualifier).index = (yyvsp[0].expression);
      }

      if ((state->has_420pack_or_es31() ||
           state->has_atomic_counters() ||
           state->has_shader_storage_buffer_objects()) &&
          match_layout_qualifier("binding", (yyvsp[-2].identifier), state) == 0) {
         (yyval.type_qualifier).flags.q.explicit_binding = 1;
         (yyval.type_qualifier).binding = (yyvsp[0].expression);
      }

      if ((state->has_atomic_counters() ||
           state->has_enhanced_layouts()) &&
          match_layout_qualifier("offset", (yyvsp[-2].identifier), state) == 0) {
         (yyval.type_qualifier).flags.q.explicit_offset = 1;
         (yyval.type_qualifier).offset = (yyvsp[0].expression);
      }

      if (match_layout_qualifier("max_vertices", (yyvsp[-2].identifier), state) == 0) {
         (yyval.type_qualifier).flags.q.max_vertices = 1;
         (yyval.type_qualifier).max_vertices = new(ctx) ast_layout_expression((yylsp[-2]), (yyvsp[0].expression));
         if (!state->has_geometry_shader() && !state->EXT_mesh_shader_enable) {
            _mesa_glsl_error(& (yylsp[0]), state,
                             "max_vertices qualifier specified");
         }
      }

      if (match_layout_qualifier("max_primitives", (yyvsp[-2].identifier), state) == 0) {
         (yyval.type_qualifier).flags.q.max_primitives = 1;
         (yyval.type_qualifier).max_primitives = new(ctx) ast_layout_expression((yylsp[-2]), (yyvsp[0].expression));
         if (!state->EXT_mesh_shader_enable) {
            _mesa_glsl_error(& (yylsp[0]), state,
                             "max_primitives qualifier specified");
         }
      }

      if (state->stage == MESA_SHADER_GEOMETRY) {
         if (match_layout_qualifier("stream", (yyvsp[-2].identifier), state) == 0 &&
             state->check_explicit_attrib_stream_allowed(& (yylsp[0]))) {
            (yyval.type_qualifier).flags.q.stream = 1;
            (yyval.type_qualifier).flags.q.explicit_stream = 1;
            (yyval.type_qualifier).stream = (yyvsp[0].expression);
         }
      }

      if (state->has_enhanced_layouts()) {
         if (match_layout_qualifier("xfb_buffer", (yyvsp[-2].identifier), state) == 0) {
            (yyval.type_qualifier).flags.q.xfb_buffer = 1;
            (yyval.type_qualifier).flags.q.explicit_xfb_buffer = 1;
            (yyval.type_qualifier).xfb_buffer = (yyvsp[0].expression);
         }

         if (match_layout_qualifier("xfb_offset", (yyvsp[-2].identifier), state) == 0) {
            (yyval.type_qualifier).flags.q.explicit_xfb_offset = 1;
            (yyval.type_qualifier).offset = (yyvsp[0].expression);
         }

         if (match_layout_qualifier("xfb_stride", (yyvsp[-2].identifier), state) == 0) {
            (yyval.type_qualifier).flags.q.xfb_stride = 1;
            (yyval.type_qualifier).flags.q.explicit_xfb_stride = 1;
            (yyval.type_qualifier).xfb_stride = (yyvsp[0].expression);
         }
      }

      static const char * const local_size_qualifiers[3] = {
         "local_size_x",
         "local_size_y",
         "local_size_z",
      };
      for (int i = 0; i < 3; i++) {
         if (match_layout_qualifier(local_size_qualifiers[i], (yyvsp[-2].identifier),
                                    state) == 0) {
            if (!state->has_compute_shader() && !state->EXT_mesh_shader_enable) {
               _mesa_glsl_error(& (yylsp[0]), state,
                                "%s qualifier requires GLSL 4.30 or "
                                "GLSL ES 3.10 or ARB_compute_shader or EXT_mesh_shader",
                                local_size_qualifiers[i]);
               YYERROR;
            } else {
               (yyval.type_qualifier).flags.q.local_size |= (1 << i);
               (yyval.type_qualifier).local_size[i] = new(ctx) ast_layout_expression((yylsp[-2]), (yyvsp[0].expression));
            }
            break;
         }
      }

      if (match_layout_qualifier("invocations", (yyvsp[-2].identifier), state) == 0) {
         (yyval.type_qualifier).flags.q.invocations = 1;
         (yyval.type_qualifier).invocations = new(ctx) ast_layout_expression((yylsp[-2]), (yyvsp[0].expression));
         if (!state->is_version(400, 320) &&
             !state->ARB_gpu_shader5_enable &&
             !state->OES_geometry_shader_enable &&
             !state->EXT_geometry_shader_enable) {
            _mesa_glsl_error(& (yylsp[0]), state,
                             "GL_ARB_gpu_shader5 invocations "
                             "qualifier specified");
         }
      }

      /* Layout qualifiers for tessellation control shaders. */
      if (match_layout_qualifier("vertices", (yyvsp[-2].identifier), state) == 0) {
         (yyval.type_qualifier).flags.q.vertices = 1;
         (yyval.type_qualifier).vertices = new(ctx) ast_layout_expression((yylsp[-2]), (yyvsp[0].expression));
         if (!state->has_tessellation_shader()) {
            _mesa_glsl_error(& (yylsp[-2]), state,
                             "vertices qualifier requires GLSL 4.00 or "
                             "ARB_tessellation_shader");
         }
      }

      /* If the identifier didn't match any known layout identifiers,
       * emit an error.
       */
      if (!(yyval.type_qualifier).flags.i) {
         _mesa_glsl_error(& (yylsp[-2]), state, "unrecognized layout identifier "
                          "`%s'", (yyvsp[-2].identifier));
         YYERROR;
      }
   }
#line 4660 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 154: /* layout_qualifier_id: interface_block_layout_qualifier  */
#line 1965 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.type_qualifier) = (yyvsp[0].type_qualifier);
      /* Layout qualifiers for ARB_uniform_buffer_object. */
      if ((yyval.type_qualifier).flags.q.uniform && !state->has_uniform_buffer_objects()) {
         _mesa_glsl_error(& (yylsp[0]), state,
                          "#version 140 / GL_ARB_uniform_buffer_object "
                          "layout qualifier `uniform' is used");
      } else if ((yyval.type_qualifier).flags.q.uniform && state->ARB_uniform_buffer_object_warn) {
         _mesa_glsl_warning(& (yylsp[0]), state,
                            "#version 140 / GL_ARB_uniform_buffer_object "
                            "layout qualifier `uniform' is used");
      }
   }
#line 4678 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 155: /* interface_block_layout_qualifier: ROW_MAJOR  */
#line 1991 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.row_major = 1;
   }
#line 4687 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 156: /* interface_block_layout_qualifier: PACKED_TOK  */
#line 1996 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.packed = 1;
   }
#line 4696 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 157: /* interface_block_layout_qualifier: SHARED  */
#line 2001 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.shared = 1;
   }
#line 4705 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 158: /* subroutine_qualifier: SUBROUTINE  */
#line 2009 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.subroutine = 1;
   }
#line 4714 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 159: /* subroutine_qualifier: SUBROUTINE '(' subroutine_type_list ')'  */
#line 2014 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.subroutine = 1;
      (yyval.type_qualifier).subroutine_list = (yyvsp[-1].subroutine_list);
   }
#line 4724 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 160: /* subroutine_type_list: any_identifier  */
#line 2023 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
        linear_ctx *ctx = state->linalloc;
        ast_declaration *decl = new(ctx)  ast_declaration((yyvsp[0].identifier), NULL, NULL);
        decl->set_location((yylsp[0]));

        (yyval.subroutine_list) = new(ctx) ast_subroutine_list();
        (yyval.subroutine_list)->declarations.push_tail(&decl->link);
   }
#line 4737 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 161: /* subroutine_type_list: subroutine_type_list ',' any_identifier  */
#line 2032 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
        linear_ctx *ctx = state->linalloc;
        ast_declaration *decl = new(ctx)  ast_declaration((yyvsp[0].identifier), NULL, NULL);
        decl->set_location((yylsp[0]));

        (yyval.subroutine_list) = (yyvsp[-2].subroutine_list);
        (yyval.subroutine_list)->declarations.push_tail(&decl->link);
   }
#line 4750 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 162: /* interpolation_qualifier: SMOOTH  */
#line 2044 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.smooth = 1;
   }
#line 4759 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 163: /* interpolation_qualifier: FLAT  */
#line 2049 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.flat = 1;
   }
#line 4768 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 164: /* interpolation_qualifier: NOPERSPECTIVE  */
#line 2054 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.noperspective = 1;
   }
#line 4777 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 165: /* type_qualifier: INVARIANT  */
#line 2063 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.invariant = 1;
   }
#line 4786 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 166: /* type_qualifier: PRECISE  */
#line 2068 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.precise = 1;
   }
#line 4795 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 173: /* type_qualifier: precision_qualifier  */
#line 2079 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(&(yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).precision = (yyvsp[0].n);
   }
#line 4804 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 174: /* type_qualifier: PRECISE type_qualifier  */
#line 2097 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      if ((yyvsp[0].type_qualifier).flags.q.precise)
         _mesa_glsl_error(&(yylsp[-1]), state, "duplicate \"precise\" qualifier");

      (yyval.type_qualifier) = (yyvsp[0].type_qualifier);
      (yyval.type_qualifier).flags.q.precise = 1;
   }
#line 4816 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 175: /* type_qualifier: INVARIANT type_qualifier  */
#line 2105 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      if ((yyvsp[0].type_qualifier).flags.q.invariant)
         _mesa_glsl_error(&(yylsp[-1]), state, "duplicate \"invariant\" qualifier");

      if (!state->has_420pack_or_es31() && (yyvsp[0].type_qualifier).flags.q.precise)
         _mesa_glsl_error(&(yylsp[-1]), state,
                          "\"invariant\" must come after \"precise\"");

      (yyval.type_qualifier) = (yyvsp[0].type_qualifier);
      (yyval.type_qualifier).flags.q.invariant = 1;

      /* GLSL ES 3.00 spec, section 4.6.1 "The Invariant Qualifier":
       *
       * "Only variables output from a shader can be candidates for invariance.
       * This includes user-defined output variables and the built-in output
       * variables. As only outputs can be declared as invariant, an invariant
       * output from one shader stage will still match an input of a subsequent
       * stage without the input being declared as invariant."
       *
       * On the desktop side, this text first appears in GLSL 4.20.
       */
      if (state->is_version(420, 300) && (yyval.type_qualifier).flags.q.in)
         _mesa_glsl_error(&(yylsp[-1]), state, "invariant qualifiers cannot be used with shader inputs");
   }
#line 4845 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 176: /* type_qualifier: interpolation_qualifier type_qualifier  */
#line 2130 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      /* Section 4.3 of the GLSL 1.40 specification states:
       * "...qualified with one of these interpolation qualifiers"
       *
       * GLSL 1.30 claims to allow "one or more", but insists that:
       * "These interpolation qualifiers may only precede the qualifiers in,
       *  centroid in, out, or centroid out in a declaration."
       *
       * ...which means that e.g. smooth can't precede smooth, so there can be
       * only one after all, and the 1.40 text is a clarification, not a change.
       */
      if ((yyvsp[0].type_qualifier).has_interpolation())
         _mesa_glsl_error(&(yylsp[-1]), state, "duplicate interpolation qualifier");

      if (!state->has_420pack_or_es31() &&
          ((yyvsp[0].type_qualifier).flags.q.precise || (yyvsp[0].type_qualifier).flags.q.invariant)) {
         _mesa_glsl_error(&(yylsp[-1]), state, "interpolation qualifiers must come "
                          "after \"precise\" or \"invariant\"");
      }

      (yyval.type_qualifier) = (yyvsp[-1].type_qualifier);
      (yyval.type_qualifier).merge_qualifier(&(yylsp[-1]), state, (yyvsp[0].type_qualifier), false);
   }
#line 4873 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 177: /* type_qualifier: layout_qualifier type_qualifier  */
#line 2154 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      /* In the absence of ARB_shading_language_420pack, layout qualifiers may
       * appear no later than auxiliary storage qualifiers. There is no
       * particularly clear spec language mandating this, but in all examples
       * the layout qualifier precedes the storage qualifier.
       *
       * We allow combinations of layout with interpolation, invariant or
       * precise qualifiers since these are useful in ARB_separate_shader_objects.
       * There is no clear spec guidance on this either.
       */
      (yyval.type_qualifier) = (yyvsp[-1].type_qualifier);
      (yyval.type_qualifier).merge_qualifier(& (yylsp[-1]), state, (yyvsp[0].type_qualifier), false, (yyvsp[0].type_qualifier).has_layout());
   }
#line 4891 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 178: /* type_qualifier: subroutine_qualifier type_qualifier  */
#line 2168 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.type_qualifier) = (yyvsp[-1].type_qualifier);
      (yyval.type_qualifier).merge_qualifier(&(yylsp[-1]), state, (yyvsp[0].type_qualifier), false);
   }
#line 4900 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 179: /* type_qualifier: auxiliary_storage_qualifier type_qualifier  */
#line 2173 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      if ((yyvsp[0].type_qualifier).has_auxiliary_storage()) {
         _mesa_glsl_error(&(yylsp[-1]), state,
                          "duplicate auxiliary storage qualifier (centroid or sample)");
      }

      if ((!state->has_420pack_or_es31() && !state->EXT_gpu_shader4_enable) &&
          ((yyvsp[0].type_qualifier).flags.q.precise || (yyvsp[0].type_qualifier).flags.q.invariant ||
           (yyvsp[0].type_qualifier).has_interpolation() || (yyvsp[0].type_qualifier).has_layout())) {
         _mesa_glsl_error(&(yylsp[-1]), state, "auxiliary storage qualifiers must come "
                          "just before storage qualifiers");
      }
      (yyval.type_qualifier) = (yyvsp[-1].type_qualifier);
      (yyval.type_qualifier).merge_qualifier(&(yylsp[-1]), state, (yyvsp[0].type_qualifier), false);
   }
#line 4920 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 180: /* type_qualifier: storage_qualifier type_qualifier  */
#line 2189 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      /* Section 4.3 of the GLSL 1.20 specification states:
       * "Variable declarations may have a storage qualifier specified..."
       *  1.30 clarifies this to "may have one storage qualifier".
       *
       * GL_EXT_gpu_shader4 allows "varying out" in fragment shaders.
       */
      if ((yyvsp[0].type_qualifier).has_storage() &&
          (!state->EXT_gpu_shader4_enable ||
           state->stage != MESA_SHADER_FRAGMENT ||
           !(yyvsp[-1].type_qualifier).flags.q.varying || !(yyvsp[0].type_qualifier).flags.q.out))
         _mesa_glsl_error(&(yylsp[-1]), state, "duplicate storage qualifier");

      if (!state->has_420pack_or_es31() &&
          ((yyvsp[0].type_qualifier).flags.q.precise || (yyvsp[0].type_qualifier).flags.q.invariant || (yyvsp[0].type_qualifier).has_interpolation() ||
           (yyvsp[0].type_qualifier).has_layout() || (yyvsp[0].type_qualifier).has_auxiliary_storage())) {
         _mesa_glsl_error(&(yylsp[-1]), state, "storage qualifiers must come after "
                          "precise, invariant, interpolation, layout and auxiliary "
                          "storage qualifiers");
      }

      (yyval.type_qualifier) = (yyvsp[-1].type_qualifier);
      (yyval.type_qualifier).merge_qualifier(&(yylsp[-1]), state, (yyvsp[0].type_qualifier), false);
   }
#line 4949 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 181: /* type_qualifier: precision_qualifier type_qualifier  */
#line 2214 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      if ((yyvsp[0].type_qualifier).precision != ast_precision_none)
         _mesa_glsl_error(&(yylsp[-1]), state, "duplicate precision qualifier");

      if (!(state->has_420pack_or_es31()) &&
          (yyvsp[0].type_qualifier).flags.i != 0)
         _mesa_glsl_error(&(yylsp[-1]), state, "precision qualifiers must come last");

      (yyval.type_qualifier) = (yyvsp[0].type_qualifier);
      (yyval.type_qualifier).precision = (yyvsp[-1].n);
   }
#line 4965 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 182: /* type_qualifier: memory_qualifier type_qualifier  */
#line 2226 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.type_qualifier) = (yyvsp[-1].type_qualifier);
      (yyval.type_qualifier).merge_qualifier(&(yylsp[-1]), state, (yyvsp[0].type_qualifier), false);
   }
#line 4974 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 183: /* auxiliary_storage_qualifier: CENTROID  */
#line 2234 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.centroid = 1;
   }
#line 4983 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 184: /* auxiliary_storage_qualifier: SAMPLE  */
#line 2239 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.sample = 1;
   }
#line 4992 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 185: /* auxiliary_storage_qualifier: PATCH  */
#line 2244 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.patch = 1;
   }
#line 5001 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 186: /* auxiliary_storage_qualifier: PERPRIMITIVE  */
#line 2249 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.per_primitive = 1;
   }
#line 5010 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 187: /* storage_qualifier: CONST_TOK  */
#line 2256 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.constant = 1;
   }
#line 5019 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 188: /* storage_qualifier: ATTRIBUTE  */
#line 2261 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.attribute = 1;
   }
#line 5028 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 189: /* storage_qualifier: VARYING  */
#line 2266 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.varying = 1;
   }
#line 5037 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 190: /* storage_qualifier: IN_TOK  */
#line 2271 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.in = 1;
   }
#line 5046 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 191: /* storage_qualifier: OUT_TOK  */
#line 2276 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.out = 1;

      if (state->stage == MESA_SHADER_GEOMETRY &&
          state->has_explicit_attrib_stream()) {
         /* Section 4.3.8.2 (Output Layout Qualifiers) of the GLSL 4.00
          * spec says:
          *
          *     "If the block or variable is declared with the stream
          *     identifier, it is associated with the specified stream;
          *     otherwise, it is associated with the current default stream."
          */
          (yyval.type_qualifier).flags.q.stream = 1;
          (yyval.type_qualifier).flags.q.explicit_stream = 0;
          (yyval.type_qualifier).stream = state->out_qualifier->stream;
      }

      if (state->has_enhanced_layouts() && state->exts->ARB_transform_feedback3) {
          (yyval.type_qualifier).flags.q.xfb_buffer = 1;
          (yyval.type_qualifier).flags.q.explicit_xfb_buffer = 0;
          (yyval.type_qualifier).xfb_buffer = state->out_qualifier->xfb_buffer;
      }
   }
#line 5075 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 192: /* storage_qualifier: INOUT_TOK  */
#line 2301 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.in = 1;
      (yyval.type_qualifier).flags.q.out = 1;

      if (!state->has_framebuffer_fetch() ||
          !state->is_version(130, 300) ||
          state->stage != MESA_SHADER_FRAGMENT)
         _mesa_glsl_error(&(yylsp[0]), state, "A single interface variable cannot be "
                          "declared as both input and output");
   }
#line 5091 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 193: /* storage_qualifier: UNIFORM  */
#line 2313 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.uniform = 1;
   }
#line 5100 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 194: /* storage_qualifier: BUFFER  */
#line 2318 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.buffer = 1;
   }
#line 5109 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 195: /* storage_qualifier: SHARED  */
#line 2323 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.shared_storage = 1;
   }
#line 5118 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 196: /* storage_qualifier: TASKPAYLOAD  */
#line 2328 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.task_payload = 1;
   }
#line 5127 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 197: /* storage_qualifier: PIXEL_LOCAL  */
#line 2333 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.pixel_local_storage = GLSL_PIXEL_LOCAL_STORAGE_INOUT;
   }
#line 5136 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 198: /* storage_qualifier: PIXEL_LOCAL_IN  */
#line 2338 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.pixel_local_storage = GLSL_PIXEL_LOCAL_STORAGE_IN;
   }
#line 5145 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 199: /* storage_qualifier: PIXEL_LOCAL_OUT  */
#line 2343 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.pixel_local_storage = GLSL_PIXEL_LOCAL_STORAGE_OUT;
   }
#line 5154 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 200: /* memory_qualifier: COHERENT  */
#line 2351 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.coherent = 1;
   }
#line 5163 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 201: /* memory_qualifier: VOLATILE  */
#line 2356 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q._volatile = 1;
   }
#line 5172 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 202: /* memory_qualifier: RESTRICT  */
#line 2361 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      STATIC_ASSERT(sizeof((yyval.type_qualifier).flags.q) <= sizeof((yyval.type_qualifier).flags.i));
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.restrict_flag = 1;
   }
#line 5182 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 203: /* memory_qualifier: READONLY  */
#line 2367 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.read_only = 1;
   }
#line 5191 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 204: /* memory_qualifier: WRITEONLY  */
#line 2372 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.write_only = 1;
   }
#line 5200 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 205: /* array_specifier: '[' ']'  */
#line 2380 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.array_specifier) = new(ctx) ast_array_specifier((yylsp[-1]), new(ctx) ast_expression(
                                                  ast_unsized_array_dim, NULL,
                                                  NULL, NULL));
      (yyval.array_specifier)->set_location_range((yylsp[-1]), (yylsp[0]));
   }
#line 5212 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 206: /* array_specifier: '[' constant_expression ']'  */
#line 2388 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.array_specifier) = new(ctx) ast_array_specifier((yylsp[-2]), (yyvsp[-1].expression));
      (yyval.array_specifier)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 5222 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 207: /* array_specifier: array_specifier '[' ']'  */
#line 2394 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.array_specifier) = (yyvsp[-2].array_specifier);

      if (state->check_arrays_of_arrays_allowed(& (yylsp[-2]))) {
         (yyval.array_specifier)->add_dimension(new(ctx) ast_expression(ast_unsized_array_dim, NULL,
                                                   NULL, NULL));
      }
   }
#line 5236 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 208: /* array_specifier: array_specifier '[' constant_expression ']'  */
#line 2404 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.array_specifier) = (yyvsp[-3].array_specifier);

      if (state->check_arrays_of_arrays_allowed(& (yylsp[-3]))) {
         (yyval.array_specifier)->add_dimension((yyvsp[-1].expression));
      }
   }
#line 5248 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 210: /* type_specifier: type_specifier_nonarray array_specifier  */
#line 2416 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.type_specifier) = (yyvsp[-1].type_specifier);
      (yyval.type_specifier)->array_specifier = (yyvsp[0].array_specifier);
   }
#line 5257 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 211: /* type_specifier_nonarray: basic_type_specifier_nonarray  */
#line 2424 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.type_specifier) = new(ctx) ast_type_specifier((yyvsp[0].type));
      (yyval.type_specifier)->set_location((yylsp[0]));
   }
#line 5267 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 212: /* type_specifier_nonarray: struct_specifier  */
#line 2430 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.type_specifier) = new(ctx) ast_type_specifier((yyvsp[0].struct_specifier));
      (yyval.type_specifier)->set_location((yylsp[0]));
   }
#line 5277 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 213: /* type_specifier_nonarray: TYPE_IDENTIFIER  */
#line 2436 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.type_specifier) = new(ctx) ast_type_specifier((yyvsp[0].identifier));
      (yyval.type_specifier)->set_location((yylsp[0]));
   }
#line 5287 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 214: /* basic_type_specifier_nonarray: VOID_TOK  */
#line 2444 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                            { (yyval.type) = &glsl_type_builtin_void; }
#line 5293 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 215: /* basic_type_specifier_nonarray: BASIC_TYPE_TOK  */
#line 2445 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                            { (yyval.type) = (yyvsp[0].type); }
#line 5299 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 216: /* basic_type_specifier_nonarray: UNSIGNED BASIC_TYPE_TOK  */
#line 2447 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      if ((yyvsp[0].type) == &glsl_type_builtin_int) {
         (yyval.type) = &glsl_type_builtin_uint;
      } else {
         _mesa_glsl_error(&(yylsp[-1]), state,
                          "\"unsigned\" is only allowed before \"int\"");
      }
   }
#line 5312 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 217: /* precision_qualifier: HIGHP  */
#line 2459 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      state->check_precision_qualifiers_allowed(&(yylsp[0]));
      (yyval.n) = ast_precision_high;
   }
#line 5321 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 218: /* precision_qualifier: MEDIUMP  */
#line 2464 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      state->check_precision_qualifiers_allowed(&(yylsp[0]));
      (yyval.n) = ast_precision_medium;
   }
#line 5330 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 219: /* precision_qualifier: LOWP  */
#line 2469 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      state->check_precision_qualifiers_allowed(&(yylsp[0]));
      (yyval.n) = ast_precision_low;
   }
#line 5339 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 220: /* struct_specifier: STRUCT any_identifier '{' struct_declaration_list '}'  */
#line 2477 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.struct_specifier) = new(ctx) ast_struct_specifier((yyvsp[-3].identifier), (yyvsp[-1].declarator_list));
      (yyval.struct_specifier)->set_location_range((yylsp[-3]), (yylsp[0]));
      state->symbols->add_type((yyvsp[-3].identifier), &glsl_type_builtin_void);
   }
#line 5350 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 221: /* struct_specifier: STRUCT '{' struct_declaration_list '}'  */
#line 2484 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;

      /* All anonymous structs have the same name. This simplifies matching of
       * globals whose type is an unnamed struct.
       *
       * It also avoids a memory leak when the same shader is compiled over and
       * over again.
       */
      (yyval.struct_specifier) = new(ctx) ast_struct_specifier("#anon_struct", (yyvsp[-1].declarator_list));

      (yyval.struct_specifier)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 5368 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 222: /* struct_declaration_list: struct_declaration  */
#line 2501 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.declarator_list) = (yyvsp[0].declarator_list);
      (yyvsp[0].declarator_list)->link.self_link();
   }
#line 5377 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 223: /* struct_declaration_list: struct_declaration_list struct_declaration  */
#line 2506 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.declarator_list) = (yyvsp[-1].declarator_list);
      (yyval.declarator_list)->link.insert_before(& (yyvsp[0].declarator_list)->link);
   }
#line 5386 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 224: /* struct_declaration: fully_specified_type struct_declarator_list ';'  */
#line 2514 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      ast_fully_specified_type *const type = (yyvsp[-2].fully_specified_type);
      type->set_location((yylsp[-2]));

      if (state->has_bindless()) {
         ast_type_qualifier input_layout_mask;

         /* Allow to declare qualifiers for images. */
         input_layout_mask.flags.i = 0;
         input_layout_mask.flags.q.coherent = 1;
         input_layout_mask.flags.q._volatile = 1;
         input_layout_mask.flags.q.restrict_flag = 1;
         input_layout_mask.flags.q.read_only = 1;
         input_layout_mask.flags.q.write_only = 1;
         input_layout_mask.flags.q.explicit_image_format = 1;

         if ((type->qualifier.flags.i & ~input_layout_mask.flags.i) != 0) {
            _mesa_glsl_error(&(yylsp[-2]), state,
                             "only precision and image qualifiers may be "
                             "applied to structure members");
         }
      } else {
         if (type->qualifier.flags.i != 0)
            _mesa_glsl_error(&(yylsp[-2]), state,
                             "only precision qualifiers may be applied to "
                             "structure members");
      }

      (yyval.declarator_list) = new(ctx) ast_declarator_list(type);
      (yyval.declarator_list)->set_location((yylsp[-1]));

      (yyval.declarator_list)->declarations.push_degenerate_list_at_head(& (yyvsp[-1].declaration)->link);
   }
#line 5425 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 225: /* struct_declarator_list: struct_declarator  */
#line 2552 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.declaration) = (yyvsp[0].declaration);
      (yyvsp[0].declaration)->link.self_link();
   }
#line 5434 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 226: /* struct_declarator_list: struct_declarator_list ',' struct_declarator  */
#line 2557 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.declaration) = (yyvsp[-2].declaration);
      (yyval.declaration)->link.insert_before(& (yyvsp[0].declaration)->link);
   }
#line 5443 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 227: /* struct_declarator: any_identifier  */
#line 2565 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.declaration) = new(ctx) ast_declaration((yyvsp[0].identifier), NULL, NULL);
      (yyval.declaration)->set_location((yylsp[0]));
   }
#line 5453 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 228: /* struct_declarator: any_identifier array_specifier  */
#line 2571 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.declaration) = new(ctx) ast_declaration((yyvsp[-1].identifier), (yyvsp[0].array_specifier), NULL);
      (yyval.declaration)->set_location_range((yylsp[-1]), (yylsp[0]));
   }
#line 5463 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 230: /* initializer: '{' initializer_list '}'  */
#line 2581 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.expression) = (yyvsp[-1].expression);
   }
#line 5471 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 231: /* initializer: '{' initializer_list ',' '}'  */
#line 2585 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.expression) = (yyvsp[-2].expression);
   }
#line 5479 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 232: /* initializer_list: initializer  */
#line 2592 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_aggregate_initializer();
      (yyval.expression)->set_location((yylsp[0]));
      (yyval.expression)->expressions.push_tail(& (yyvsp[0].expression)->link);
   }
#line 5490 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 233: /* initializer_list: initializer_list ',' initializer  */
#line 2599 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyvsp[-2].expression)->expressions.push_tail(& (yyvsp[0].expression)->link);
   }
#line 5498 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 235: /* statement: compound_statement  */
#line 2611 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                             { (yyval.node) = (ast_node *) (yyvsp[0].compound_statement); }
#line 5504 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 244: /* compound_statement: '{' '}'  */
#line 2627 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.compound_statement) = new(ctx) ast_compound_statement(true, NULL);
      (yyval.compound_statement)->set_location_range((yylsp[-1]), (yylsp[0]));
   }
#line 5514 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 245: /* $@2: %empty  */
#line 2633 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      state->symbols->push_scope();
   }
#line 5522 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 246: /* compound_statement: '{' $@2 statement_list '}'  */
#line 2637 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.compound_statement) = new(ctx) ast_compound_statement(true, (yyvsp[-1].node));
      (yyval.compound_statement)->set_location_range((yylsp[-3]), (yylsp[0]));
      state->symbols->pop_scope();
   }
#line 5533 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 247: /* statement_no_new_scope: compound_statement_no_new_scope  */
#line 2646 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                                   { (yyval.node) = (ast_node *) (yyvsp[0].compound_statement); }
#line 5539 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 249: /* compound_statement_no_new_scope: '{' '}'  */
#line 2652 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.compound_statement) = new(ctx) ast_compound_statement(false, NULL);
      (yyval.compound_statement)->set_location_range((yylsp[-1]), (yylsp[0]));
   }
#line 5549 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 250: /* compound_statement_no_new_scope: '{' statement_list '}'  */
#line 2658 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.compound_statement) = new(ctx) ast_compound_statement(false, (yyvsp[-1].node));
      (yyval.compound_statement)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 5559 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 251: /* statement_list: statement  */
#line 2667 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      if ((yyvsp[0].node) == NULL) {
         _mesa_glsl_error(& (yylsp[0]), state, "<nil> statement");
         assert((yyvsp[0].node) != NULL);
      }

      (yyval.node) = (yyvsp[0].node);
      (yyval.node)->link.self_link();
   }
#line 5573 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 252: /* statement_list: statement_list statement  */
#line 2677 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      if ((yyvsp[0].node) == NULL) {
         _mesa_glsl_error(& (yylsp[0]), state, "<nil> statement");
         assert((yyvsp[0].node) != NULL);
      }
      (yyval.node) = (yyvsp[-1].node);
      (yyval.node)->link.insert_before(& (yyvsp[0].node)->link);
   }
#line 5586 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 253: /* statement_list: statement_list extension_statement  */
#line 2686 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      if (!state->allow_extension_directive_midshader) {
         _mesa_glsl_error(& (yylsp[-1]), state,
                          "#extension directive is not allowed "
                          "in the middle of a shader");
         YYERROR;
      }
   }
#line 5599 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 254: /* expression_statement: ';'  */
#line 2698 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_expression_statement(NULL);
      (yyval.node)->set_location((yylsp[0]));
   }
#line 5609 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 255: /* expression_statement: expression ';'  */
#line 2704 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_expression_statement((yyvsp[-1].expression));
      (yyval.node)->set_location((yylsp[-1]));
   }
#line 5619 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 256: /* selection_statement: IF '(' expression ')' selection_rest_statement  */
#line 2713 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.node) = new(state->linalloc) ast_selection_statement((yyvsp[-2].expression), (yyvsp[0].selection_rest_statement).then_statement,
                                                        (yyvsp[0].selection_rest_statement).else_statement);
      (yyval.node)->set_location_range((yylsp[-4]), (yylsp[0]));
   }
#line 5629 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 257: /* selection_rest_statement: statement ELSE statement  */
#line 2722 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.selection_rest_statement).then_statement = (yyvsp[-2].node);
      (yyval.selection_rest_statement).else_statement = (yyvsp[0].node);
   }
#line 5638 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 258: /* selection_rest_statement: statement  */
#line 2727 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.selection_rest_statement).then_statement = (yyvsp[0].node);
      (yyval.selection_rest_statement).else_statement = NULL;
   }
#line 5647 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 259: /* condition: expression  */
#line 2735 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.node) = (ast_node *) (yyvsp[0].expression);
   }
#line 5655 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 260: /* condition: fully_specified_type any_identifier '=' initializer  */
#line 2739 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[-2].identifier), NULL, (yyvsp[0].expression));
      ast_declarator_list *declarator = new(ctx) ast_declarator_list((yyvsp[-3].fully_specified_type));
      decl->set_location_range((yylsp[-2]), (yylsp[0]));
      declarator->set_location((yylsp[-3]));

      declarator->declarations.push_tail(&decl->link);
      (yyval.node) = declarator;
   }
#line 5670 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 261: /* switch_statement: SWITCH '(' expression ')' switch_body  */
#line 2757 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.node) = new(state->linalloc) ast_switch_statement((yyvsp[-2].expression), (yyvsp[0].switch_body));
      (yyval.node)->set_location_range((yylsp[-4]), (yylsp[0]));
   }
#line 5679 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 262: /* switch_body: '{' '}'  */
#line 2765 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.switch_body) = new(state->linalloc) ast_switch_body(NULL);
      (yyval.switch_body)->set_location_range((yylsp[-1]), (yylsp[0]));
   }
#line 5688 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 263: /* switch_body: '{' case_statement_list '}'  */
#line 2770 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.switch_body) = new(state->linalloc) ast_switch_body((yyvsp[-1].case_statement_list));
      (yyval.switch_body)->set_location_range((yylsp[-2]), (yylsp[0]));
   }
#line 5697 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 264: /* case_label: CASE expression ':'  */
#line 2778 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.case_label) = new(state->linalloc) ast_case_label((yyvsp[-1].expression));
      (yyval.case_label)->set_location((yylsp[-1]));
   }
#line 5706 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 265: /* case_label: DEFAULT ':'  */
#line 2783 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.case_label) = new(state->linalloc) ast_case_label(NULL);
      (yyval.case_label)->set_location((yylsp[0]));
   }
#line 5715 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 266: /* case_label_list: case_label  */
#line 2791 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      ast_case_label_list *labels = new(state->linalloc) ast_case_label_list();

      labels->labels.push_tail(& (yyvsp[0].case_label)->link);
      (yyval.case_label_list) = labels;
      (yyval.case_label_list)->set_location((yylsp[0]));
   }
#line 5727 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 267: /* case_label_list: case_label_list case_label  */
#line 2799 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.case_label_list) = (yyvsp[-1].case_label_list);
      (yyval.case_label_list)->labels.push_tail(& (yyvsp[0].case_label)->link);
   }
#line 5736 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 268: /* case_statement: case_label_list statement  */
#line 2807 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      ast_case_statement *stmts = new(state->linalloc) ast_case_statement((yyvsp[-1].case_label_list));
      stmts->set_location((yylsp[0]));

      stmts->stmts.push_tail(& (yyvsp[0].node)->link);
      (yyval.case_statement) = stmts;
   }
#line 5748 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 269: /* case_statement: case_statement statement  */
#line 2815 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.case_statement) = (yyvsp[-1].case_statement);
      (yyval.case_statement)->stmts.push_tail(& (yyvsp[0].node)->link);
   }
#line 5757 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 270: /* case_statement_list: case_statement  */
#line 2823 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      ast_case_statement_list *cases= new(state->linalloc) ast_case_statement_list();
      cases->set_location((yylsp[0]));

      cases->cases.push_tail(& (yyvsp[0].case_statement)->link);
      (yyval.case_statement_list) = cases;
   }
#line 5769 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 271: /* case_statement_list: case_statement_list case_statement  */
#line 2831 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.case_statement_list) = (yyvsp[-1].case_statement_list);
      (yyval.case_statement_list)->cases.push_tail(& (yyvsp[0].case_statement)->link);
   }
#line 5778 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 272: /* iteration_statement: WHILE '(' condition ')' statement_no_new_scope  */
#line 2839 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_iteration_statement(ast_iteration_statement::ast_while,
                                            NULL, (yyvsp[-2].node), NULL, (yyvsp[0].node));
      (yyval.node)->set_location_range((yylsp[-4]), (yylsp[-1]));
   }
#line 5789 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 273: /* iteration_statement: DO statement_no_new_scope WHILE '(' expression ')' ';'  */
#line 2846 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_iteration_statement(ast_iteration_statement::ast_do_while,
                                            NULL, (yyvsp[-2].expression), NULL, (yyvsp[-5].node));
      (yyval.node)->set_location_range((yylsp[-6]), (yylsp[-1]));
   }
#line 5800 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 274: /* iteration_statement: FOR '(' for_init_statement for_rest_statement ')' statement_no_new_scope  */
#line 2853 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_iteration_statement(ast_iteration_statement::ast_for,
                                            (yyvsp[-3].node), (yyvsp[-2].for_rest_statement).cond, (yyvsp[-2].for_rest_statement).rest, (yyvsp[0].node));
      (yyval.node)->set_location_range((yylsp[-5]), (yylsp[0]));
   }
#line 5811 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 278: /* conditionopt: %empty  */
#line 2869 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.node) = NULL;
   }
#line 5819 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 279: /* for_rest_statement: conditionopt ';'  */
#line 2876 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.for_rest_statement).cond = (yyvsp[-1].node);
      (yyval.for_rest_statement).rest = NULL;
   }
#line 5828 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 280: /* for_rest_statement: conditionopt ';' expression  */
#line 2881 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.for_rest_statement).cond = (yyvsp[-2].node);
      (yyval.for_rest_statement).rest = (yyvsp[0].expression);
   }
#line 5837 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 281: /* jump_statement: CONTINUE ';'  */
#line 2890 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_jump_statement(ast_jump_statement::ast_continue, NULL);
      (yyval.node)->set_location((yylsp[-1]));
   }
#line 5847 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 282: /* jump_statement: BREAK ';'  */
#line 2896 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_jump_statement(ast_jump_statement::ast_break, NULL);
      (yyval.node)->set_location((yylsp[-1]));
   }
#line 5857 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 283: /* jump_statement: RETURN ';'  */
#line 2902 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_jump_statement(ast_jump_statement::ast_return, NULL);
      (yyval.node)->set_location((yylsp[-1]));
   }
#line 5867 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 284: /* jump_statement: RETURN expression ';'  */
#line 2908 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_jump_statement(ast_jump_statement::ast_return, (yyvsp[-1].expression));
      (yyval.node)->set_location_range((yylsp[-2]), (yylsp[-1]));
   }
#line 5877 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 285: /* jump_statement: DISCARD ';'  */
#line 2914 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_jump_statement(ast_jump_statement::ast_discard, NULL);
      (yyval.node)->set_location((yylsp[-1]));
   }
#line 5887 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 286: /* demote_statement: DEMOTE ';'  */
#line 2923 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_demote_statement();
      (yyval.node)->set_location((yylsp[-1]));
   }
#line 5897 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 287: /* external_declaration: function_definition  */
#line 2931 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                            { (yyval.node) = (yyvsp[0].function_definition); }
#line 5903 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 288: /* external_declaration: declaration  */
#line 2932 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                            { (yyval.node) = (yyvsp[0].node); }
#line 5909 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 289: /* external_declaration: pragma_statement  */
#line 2933 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                            { (yyval.node) = (yyvsp[0].node); }
#line 5915 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 290: /* external_declaration: layout_defaults  */
#line 2934 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                            { (yyval.node) = (yyvsp[0].node); }
#line 5921 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 291: /* external_declaration: ';'  */
#line 2935 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
                            { (yyval.node) = NULL; }
#line 5927 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 292: /* function_definition: function_prototype compound_statement_no_new_scope  */
#line 2940 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      (yyval.function_definition) = new(ctx) ast_function_definition();
      (yyval.function_definition)->set_location_range((yylsp[-1]), (yylsp[0]));
      (yyval.function_definition)->prototype = (yyvsp[-1].function);
      (yyval.function_definition)->body = (yyvsp[0].compound_statement);

      state->symbols->pop_scope();
   }
#line 5941 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 293: /* interface_block: basic_interface_block  */
#line 2954 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.node) = (yyvsp[0].interface_block);
   }
#line 5949 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 294: /* interface_block: layout_qualifier interface_block  */
#line 2958 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      ast_interface_block *block = (ast_interface_block *) (yyvsp[0].node);

      if (!(yyvsp[-1].type_qualifier).merge_qualifier(& (yylsp[-1]), state, block->layout, false,
                              block->layout.has_layout())) {
         YYERROR;
      }

      block->layout = (yyvsp[-1].type_qualifier);

      (yyval.node) = block;
   }
#line 5966 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 295: /* interface_block: memory_qualifier interface_block  */
#line 2971 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      ast_interface_block *block = (ast_interface_block *)(yyvsp[0].node);

      if (!block->default_layout.flags.q.buffer) {
            _mesa_glsl_error(& (yylsp[-1]), state,
                             "memory qualifiers can only be used in the "
                             "declaration of shader storage blocks");
      }
      if (!(yyvsp[-1].type_qualifier).merge_qualifier(& (yylsp[-1]), state, block->layout, false)) {
         YYERROR;
      }
      block->layout = (yyvsp[-1].type_qualifier);
      (yyval.node) = block;
   }
#line 5985 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 296: /* basic_interface_block: interface_qualifier NEW_IDENTIFIER '{' member_list '}' instance_name_opt ';'  */
#line 2989 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      ast_interface_block *const block = (yyvsp[-1].interface_block);

      if ((yyvsp[-6].type_qualifier).flags.q.uniform) {
         block->default_layout = *state->default_uniform_qualifier;
      } else if ((yyvsp[-6].type_qualifier).flags.q.buffer) {
         block->default_layout = *state->default_shader_storage_qualifier;
      }
      block->block_name = (yyvsp[-5].identifier);
      block->declarations.push_degenerate_list_at_head(& (yyvsp[-3].declarator_list)->link);

      _mesa_ast_process_interface_block(& (yylsp[-6]), state, block, (yyvsp[-6].type_qualifier));

      (yyval.interface_block) = block;
   }
#line 6005 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 297: /* interface_qualifier: IN_TOK  */
#line 3008 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.in = 1;
   }
#line 6014 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 298: /* interface_qualifier: OUT_TOK  */
#line 3013 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.out = 1;
   }
#line 6023 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 299: /* interface_qualifier: UNIFORM  */
#line 3018 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.uniform = 1;
   }
#line 6032 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 300: /* interface_qualifier: BUFFER  */
#line 3023 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.buffer = 1;
   }
#line 6041 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 301: /* interface_qualifier: PIXEL_LOCAL  */
#line 3028 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.pixel_local_storage = GLSL_PIXEL_LOCAL_STORAGE_INOUT;
   }
#line 6050 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 302: /* interface_qualifier: PIXEL_LOCAL_IN  */
#line 3033 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.pixel_local_storage = GLSL_PIXEL_LOCAL_STORAGE_IN;
   }
#line 6059 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 303: /* interface_qualifier: PIXEL_LOCAL_OUT  */
#line 3038 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.pixel_local_storage = GLSL_PIXEL_LOCAL_STORAGE_OUT;
   }
#line 6068 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 304: /* interface_qualifier: auxiliary_storage_qualifier interface_qualifier  */
#line 3043 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      if (!(yyvsp[-1].type_qualifier).flags.q.patch && !(yyvsp[-1].type_qualifier).flags.q.per_primitive) {
         _mesa_glsl_error(&(yylsp[-1]), state, "invalid interface qualifier");
      }
      if ((yyvsp[0].type_qualifier).has_auxiliary_storage()) {
         _mesa_glsl_error(&(yylsp[-1]), state, "duplicate auxiliary storage qualifier");
      }
      (yyval.type_qualifier) = (yyvsp[0].type_qualifier);
      (yyval.type_qualifier).flags.q.patch = (yyvsp[-1].type_qualifier).flags.q.patch;
      (yyval.type_qualifier).flags.q.per_primitive = (yyvsp[-1].type_qualifier).flags.q.per_primitive;
   }
#line 6084 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 305: /* instance_name_opt: %empty  */
#line 3058 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.interface_block) = new(state->linalloc) ast_interface_block(NULL, NULL);
   }
#line 6092 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 306: /* instance_name_opt: NEW_IDENTIFIER  */
#line 3062 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.interface_block) = new(state->linalloc) ast_interface_block((yyvsp[0].identifier), NULL);
      (yyval.interface_block)->set_location((yylsp[0]));
   }
#line 6101 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 307: /* instance_name_opt: NEW_IDENTIFIER array_specifier  */
#line 3067 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.interface_block) = new(state->linalloc) ast_interface_block((yyvsp[-1].identifier), (yyvsp[0].array_specifier));
      (yyval.interface_block)->set_location_range((yylsp[-1]), (yylsp[0]));
   }
#line 6110 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 308: /* member_list: member_declaration  */
#line 3075 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.declarator_list) = (yyvsp[0].declarator_list);
      (yyvsp[0].declarator_list)->link.self_link();
   }
#line 6119 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 309: /* member_list: member_declaration member_list  */
#line 3080 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.declarator_list) = (yyvsp[-1].declarator_list);
      (yyvsp[0].declarator_list)->link.insert_before(& (yyval.declarator_list)->link);
   }
#line 6128 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 310: /* member_declaration: fully_specified_type struct_declarator_list ';'  */
#line 3088 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      linear_ctx *ctx = state->linalloc;
      ast_fully_specified_type *type = (yyvsp[-2].fully_specified_type);
      type->set_location((yylsp[-2]));

      if (type->qualifier.flags.q.attribute) {
         _mesa_glsl_error(& (yylsp[-2]), state,
                          "keyword 'attribute' cannot be used with "
                          "interface block member");
      } else if (type->qualifier.flags.q.varying) {
         _mesa_glsl_error(& (yylsp[-2]), state,
                          "keyword 'varying' cannot be used with "
                          "interface block member");
      }

      (yyval.declarator_list) = new(ctx) ast_declarator_list(type);
      (yyval.declarator_list)->set_location((yylsp[-1]));

      (yyval.declarator_list)->declarations.push_degenerate_list_at_head(& (yyvsp[-1].declaration)->link);
   }
#line 6153 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 311: /* layout_uniform_defaults: layout_qualifier layout_uniform_defaults  */
#line 3112 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.type_qualifier) = (yyvsp[-1].type_qualifier);
      if (!(yyval.type_qualifier).merge_qualifier(& (yylsp[-1]), state, (yyvsp[0].type_qualifier), false, true)) {
         YYERROR;
      }
   }
#line 6164 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 313: /* layout_buffer_defaults: layout_qualifier layout_buffer_defaults  */
#line 3123 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.type_qualifier) = (yyvsp[-1].type_qualifier);
      if (!(yyval.type_qualifier).merge_qualifier(& (yylsp[-1]), state, (yyvsp[0].type_qualifier), false, true)) {
         YYERROR;
      }
   }
#line 6175 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 315: /* layout_in_defaults: layout_qualifier layout_in_defaults  */
#line 3134 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.type_qualifier) = (yyvsp[-1].type_qualifier);
      if (!(yyval.type_qualifier).merge_qualifier(& (yylsp[-1]), state, (yyvsp[0].type_qualifier), false, true)) {
         YYERROR;
      }
      if (!(yyval.type_qualifier).validate_in_qualifier(& (yylsp[-1]), state)) {
         YYERROR;
      }
   }
#line 6189 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 316: /* layout_in_defaults: layout_qualifier IN_TOK ';'  */
#line 3144 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      if (!(yyvsp[-2].type_qualifier).validate_in_qualifier(& (yylsp[-2]), state)) {
         YYERROR;
      }
   }
#line 6199 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 317: /* layout_out_defaults: layout_qualifier layout_out_defaults  */
#line 3153 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.type_qualifier) = (yyvsp[-1].type_qualifier);
      if (!(yyval.type_qualifier).merge_qualifier(& (yylsp[-1]), state, (yyvsp[0].type_qualifier), false, true)) {
         YYERROR;
      }
      if (!(yyval.type_qualifier).validate_out_qualifier(& (yylsp[-1]), state)) {
         YYERROR;
      }
   }
#line 6213 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 318: /* layout_out_defaults: layout_qualifier OUT_TOK ';'  */
#line 3163 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      if (!(yyvsp[-2].type_qualifier).validate_out_qualifier(& (yylsp[-2]), state)) {
         YYERROR;
      }
   }
#line 6223 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 319: /* layout_defaults: layout_uniform_defaults  */
#line 3172 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.node) = NULL;
      if (!state->default_uniform_qualifier->
             merge_qualifier(& (yylsp[0]), state, (yyvsp[0].type_qualifier), false)) {
         YYERROR;
      }
      if (!state->default_uniform_qualifier->
             push_to_global(& (yylsp[0]), state)) {
         YYERROR;
      }
   }
#line 6239 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 320: /* layout_defaults: layout_buffer_defaults  */
#line 3184 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.node) = NULL;
      if (!state->default_shader_storage_qualifier->
             merge_qualifier(& (yylsp[0]), state, (yyvsp[0].type_qualifier), false)) {
         YYERROR;
      }
      if (!state->default_shader_storage_qualifier->
             push_to_global(& (yylsp[0]), state)) {
         YYERROR;
      }

      /* From the GLSL 4.50 spec, section 4.4.5:
       *
       *     "It is a compile-time error to specify the binding identifier for
       *     the global scope or for block member declarations."
       */
      if (state->default_shader_storage_qualifier->flags.q.explicit_binding) {
         _mesa_glsl_error(& (yylsp[0]), state,
                          "binding qualifier cannot be set for default layout");
      }
   }
#line 6265 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 321: /* layout_defaults: layout_in_defaults  */
#line 3206 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.node) = NULL;
      if (!(yyvsp[0].type_qualifier).merge_into_in_qualifier(& (yylsp[0]), state, (yyval.node))) {
         YYERROR;
      }
      if (!state->in_qualifier->push_to_global(& (yylsp[0]), state)) {
         YYERROR;
      }
   }
#line 6279 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;

  case 322: /* layout_defaults: layout_out_defaults  */
#line 3216 "third_party/mesa/src/compiler/glsl/glsl_parser.yy"
   {
      (yyval.node) = NULL;
      if (!(yyvsp[0].type_qualifier).merge_into_out_qualifier(& (yylsp[0]), state, (yyval.node))) {
         YYERROR;
      }
      if (!state->out_qualifier->push_to_global(& (yylsp[0]), state)) {
         YYERROR;
      }

      (void)yynerrs;
   }
#line 6295 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"
    break;


#line 6299 "ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp"

      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", YY_CAST (yysymbol_kind_t, yyr1[yyn]), &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;

  *++yyvsp = yyval;
  *++yylsp = yyloc;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYSYMBOL_YYEMPTY : YYTRANSLATE (yychar);
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
      {
        yypcontext_t yyctx
          = {yyssp, yytoken, &yylloc};
        char const *yymsgp = YY_("syntax error");
        int yysyntax_error_status;
        yysyntax_error_status = yysyntax_error (&yymsg_alloc, &yymsg, &yyctx);
        if (yysyntax_error_status == 0)
          yymsgp = yymsg;
        else if (yysyntax_error_status == -1)
          {
            if (yymsg != yymsgbuf)
              YYSTACK_FREE (yymsg);
            yymsg = YY_CAST (char *,
                             YYSTACK_ALLOC (YY_CAST (YYSIZE_T, yymsg_alloc)));
            if (yymsg)
              {
                yysyntax_error_status
                  = yysyntax_error (&yymsg_alloc, &yymsg, &yyctx);
                yymsgp = yymsg;
              }
            else
              {
                yymsg = yymsgbuf;
                yymsg_alloc = sizeof yymsgbuf;
                yysyntax_error_status = YYENOMEM;
              }
          }
        yyerror (&yylloc, state, yymsgp);
        if (yysyntax_error_status == YYENOMEM)
          YYNOMEM;
      }
    }

  yyerror_range[1] = yylloc;
  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval, &yylloc, state);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;
  ++yynerrs;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYSYMBOL_YYerror;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYSYMBOL_YYerror)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;

      yyerror_range[1] = *yylsp;
      yydestruct ("Error: popping",
                  YY_ACCESSING_SYMBOL (yystate), yyvsp, yylsp, state);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  yyerror_range[2] = yylloc;
  ++yylsp;
  YYLLOC_DEFAULT (*yylsp, yyerror_range, 2);

  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", YY_ACCESSING_SYMBOL (yyn), yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturnlab;


/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturnlab;


/*-----------------------------------------------------------.
| yyexhaustedlab -- YYNOMEM (memory exhaustion) comes here.  |
`-----------------------------------------------------------*/
yyexhaustedlab:
  yyerror (&yylloc, state, YY_("memory exhausted"));
  yyresult = 2;
  goto yyreturnlab;


/*----------------------------------------------------------.
| yyreturnlab -- parsing is finished, clean up and return.  |
`----------------------------------------------------------*/
yyreturnlab:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval, &yylloc, state);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  YY_ACCESSING_SYMBOL (+*yyssp), yyvsp, yylsp, state);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif
  if (yymsg != yymsgbuf)
    YYSTACK_FREE (yymsg);
  return yyresult;
}

