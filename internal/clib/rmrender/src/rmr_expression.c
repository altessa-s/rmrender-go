/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <rmrender/template.h>

#include "rmr_internal.h"
#include "rmr_locale.h"

typedef enum {
  END,
  NUMBER,
  NAME,
  ADD,
  SUB,
  MUL,
  DIV,
  LE,
  GE,
  EQ,
  NE,
  LT,
  GT,
  AND,
  OR,
  QUESTION,
  COLON,
  LEFT,
  RIGHT,
  NEG,
  SELECT
} operation;

typedef struct {
  operation op;
  size_t offset, length;
  double number;
} instruction;

struct rmr_expression {
  char* source;
  instruction* code;
  size_t count, stack_size;
};

typedef struct {
  char* source;
  size_t length, position, start, token_length;
  operation token;
  double number;
  rmr_locale locale;
  instruction* code;
  size_t count, maximum, stack, peak;
  unsigned depth;
  rmr_status status;
  rmr_error* error;
} parser;

/* True if c can start or continue an identifier. */
static bool letter(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/* True if c is an ASCII decimal digit. */
static bool digit(unsigned char c) { return c >= '0' && c <= '9'; }

/* True if c is ASCII whitespace. */
static bool space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
         c == '\f';
}

/* Records the first failure only; later calls while already failed are
 * no-ops. */
static void failure(parser* p, rmr_status status, const char* message) {
  if (p->status == RMR_OK)
    p->status = rmr_fail(p->error, status, p->start, message);
}

/* Scans the next token into p->token/number/token_length, skipping
 * leading whitespace. */
static void next(parser* p) {
  if (p->status != RMR_OK) return;

  while (p->position < p->length &&
         space((unsigned char)p->source[p->position]))
    p->position++;
  p->start = p->position;
  p->token_length = 0;
  if (p->position == p->length) {
    p->token = END;
    return;
  }

  unsigned char c = (unsigned char)p->source[p->position++];
  if (digit(c) || c == '.') {
    while (p->position < p->length &&
           (digit((unsigned char)p->source[p->position]) ||
            p->source[p->position] == '.'))
      p->position++;
    char* end = nullptr;
    p->number = rmr_locale_number(p->source + p->start, &end, p->locale);
    if (end != p->source + p->position) {
      failure(p, RMR_ERR_INVALID_ARGUMENT, "invalid decimal literal");
      return;
    }
    p->token = NUMBER;
  } else if (letter(c)) {
    while (p->position < p->length &&
           (letter((unsigned char)p->source[p->position]) ||
            digit((unsigned char)p->source[p->position])))
      p->position++;
    p->token = NAME;
  } else {
    unsigned char other =
        p->position < p->length ? (unsigned char)p->source[p->position] : 0;
    if ((c == '<' || c == '>' || c == '=' || c == '!') && other == '=') {
      p->position++;
      p->token = c == '<' ? LE : c == '>' ? GE : c == '=' ? EQ : NE;
    } else if ((c == '&' || c == '|') && other == c) {
      p->position++;
      p->token = c == '&' ? AND : OR;
    } else
      switch (c) {
        case '+':
          p->token = ADD;
          break;
        case '-':
          p->token = SUB;
          break;
        case '*':
          p->token = MUL;
          break;
        case '/':
          p->token = DIV;
          break;
        case '<':
          p->token = LT;
          break;
        case '>':
          p->token = GT;
          break;
        case '?':
          p->token = QUESTION;
          break;
        case ':':
          p->token = COLON;
          break;
        case '(':
          p->token = LEFT;
          break;
        case ')':
          p->token = RIGHT;
          break;
        default:
          failure(p, c >= 128 ? RMR_ERR_UNSUPPORTED : RMR_ERR_INVALID_ARGUMENT,
                  "unsupported expression character");
          return;
      }
  }

  p->token_length = p->position - p->start;
}

/* Appends one instruction to the code (when p->code is set) and tracks
 * the evaluation stack's peak depth. */
static void emit(parser* p, operation op, size_t offset, size_t length,
                 double number) {
  if (p->status != RMR_OK) return;
  if (p->count == p->maximum) {
    failure(p, RMR_ERR_LIMIT, "expression instruction limit");
    return;
  }

  if (p->code) p->code[p->count] = (instruction){op, offset, length, number};
  p->count++;

  if (op == NUMBER || op == NAME)
    p->stack++;
  else if (op == SELECT)
    p->stack -= 2;
  else if (op != NEG)
    p->stack--;

  if (p->stack > p->peak) p->peak = p->stack;
}

/* Enters one more level of recursion, failing with LIMIT at a depth of
 * 64. */
static bool enter(parser* p) {
  if (p->status != RMR_OK) return false;
  if (p->depth == 64) {
    failure(p, RMR_ERR_LIMIT, "expression nesting limit");
    return false;
  }
  p->depth++;
  return true;
}

static void ternary(parser* p);

/* Parses a unary +/-, a parenthesized expression, or a number/name
 * operand. */
static void unary(parser* p) {
  if (p->status != RMR_OK) return;

  if (p->token == ADD || p->token == SUB) {
    if (!enter(p)) return;
    operation op = p->token;
    size_t offset = p->start;
    next(p);
    unary(p);
    if (op == SUB) emit(p, NEG, offset, 0, 0);
    p->depth--;
  } else if (p->token == LEFT) {
    if (!enter(p)) return;
    next(p);
    ternary(p);
    if (p->token != RIGHT)
      failure(p, RMR_ERR_INVALID_ARGUMENT, "unclosed expression parenthesis");
    else
      next(p);
    p->depth--;
  } else if (p->token == NUMBER || p->token == NAME) {
    emit(p, p->token, p->start, p->token_length, p->number);
    next(p);
  } else
    failure(p, RMR_ERR_INVALID_ARGUMENT, "expected expression operand");
}

/* Parses a left-associative chain of * and / over unary operands. */
static void product(parser* p) {
  unary(p);
  while (p->status == RMR_OK && (p->token == MUL || p->token == DIV)) {
    operation op = p->token;
    size_t offset = p->start;
    next(p);
    unary(p);
    emit(p, op, offset, 0, 0);
  }
}

/* Parses a left-associative chain of + and - over product terms. */
static void sum(parser* p) {
  product(p);
  while (p->status == RMR_OK && (p->token == ADD || p->token == SUB)) {
    operation op = p->token;
    size_t offset = p->start;
    next(p);
    product(p);
    emit(p, op, offset, 0, 0);
  }
}

/* Parses an optional single comparison operator over sum terms. */
static void comparison(parser* p) {
  sum(p);
  if (p->status == RMR_OK && p->token >= LE && p->token <= GT) {
    operation op = p->token;
    size_t offset = p->start;
    next(p);
    sum(p);
    emit(p, op, offset, 0, 0);
  }
}

/* Parses a left-associative chain of && over comparison terms. */
static void conjunction(parser* p) {
  comparison(p);
  while (p->status == RMR_OK && p->token == AND) {
    size_t offset = p->start;
    next(p);
    comparison(p);
    emit(p, AND, offset, 0, 0);
  }
}

/* Parses a left-associative chain of || over conjunction terms. */
static void disjunction(parser* p) {
  conjunction(p);
  while (p->status == RMR_OK && p->token == OR) {
    size_t offset = p->start;
    next(p);
    conjunction(p);
    emit(p, OR, offset, 0, 0);
  }
}

/* Parses a right-associative ?: over disjunction terms. */
static void ternary(parser* p) {
  disjunction(p);

  if (p->status == RMR_OK && p->token == QUESTION) {
    if (!enter(p)) return;
    size_t offset = p->start;
    next(p);
    ternary(p);
    if (p->token != COLON)
      failure(p, RMR_ERR_INVALID_ARGUMENT, "ternary expression without colon");
    else {
      next(p);
      ternary(p);
      emit(p, SELECT, offset, 0, 0);
    }
    p->depth--;
  }
}

/* Parses a complete expression from the current position, failing if
 * trailing input remains. */
static rmr_status parse(parser* p) {
  next(p);
  ternary(p);
  if (p->status == RMR_OK && p->token != END)
    failure(p, RMR_ERR_INVALID_ARGUMENT, "expression not fully parsed");
  return p->status;
}

void rmr_expression_free(rmr_expression* p) {
  if (p) {
    free(p->source);
    free(p->code);
    free(p);
  }
}

rmr_status rmr_expression_compile(const char* source, size_t length,
                                  const rmr_limits* given, rmr_expression** out,
                                  rmr_error* e) {
  if (!source || !out)
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0,
                    "missing expression input or output");

  rmr_limits l = given ? *given : rmr_limits_default();
  if (length > 65536 || sizeof(rmr_expression) >= l.maximum_plan_bytes ||
      length >= l.maximum_plan_bytes - sizeof(rmr_expression))
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "expression source memory limit");

  char* copy = rmr_alloc(length + 1);
  if (!copy) return rmr_fail(e, RMR_ERR_OUT_OF_MEMORY, 0, nullptr);
  memcpy(copy, source, length);
  copy[length] = 0;

  rmr_locale locale = rmr_locale_create();
  if (!locale) {
    free(copy);
    return rmr_fail(e, RMR_ERR_OUT_OF_MEMORY, 0, "numeric locale unavailable");
  }

  parser p = {.source = copy,
              .length = length,
              .locale = locale,
              .maximum = l.maximum_elements,
              .error = e};
  rmr_status status = parse(&p);

  size_t bytes = sizeof(rmr_expression) + length + 1;
  if (status == RMR_OK &&
      (p.count > (l.maximum_plan_bytes - bytes) / sizeof(instruction)))
    status = rmr_fail(e, RMR_ERR_LIMIT, 0, "expression code memory limit");
  if (status == RMR_OK) {
    bytes += p.count * sizeof(instruction);
    if (p.peak > (l.maximum_plan_bytes - bytes) / sizeof(double))
      status = rmr_fail(e, RMR_ERR_LIMIT, 0, "expression stack memory limit");
  }

  rmr_expression* compiled = nullptr;
  if (status == RMR_OK) {
    compiled = rmr_calloc(1, sizeof *compiled);
    if (compiled) compiled->code = rmr_calloc(p.count, sizeof *compiled->code);
    if (!compiled || !compiled->code)
      status = rmr_fail(e, RMR_ERR_OUT_OF_MEMORY, 0, nullptr);
    else {
      compiled->count = p.count;
      compiled->stack_size = p.peak;
      parser second = {.source = copy,
                       .length = length,
                       .locale = locale,
                       .maximum = p.count,
                       .code = compiled->code,
                       .error = e};
      status = parse(&second);
    }
  }

  rmr_locale_free(locale);

  if (status != RMR_OK) {
    free(copy);
    rmr_expression_free(compiled);
    return status;
  }

  compiled->source = copy;
  *out = compiled;
  return RMR_OK;
}

rmr_status rmr_expression_evaluate(const rmr_expression* p,
                                   const rmr_binding* bindings, size_t count,
                                   double* out, rmr_error* e) {
  if (!p || !out || (count && !bindings))
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0,
                    "invalid expression evaluation");
  if (count > 256)
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "expression binding limit");

  for (size_t i = 0; i < count; i++) {
    rmr_binding b = bindings[i];
    if (!b.name || !b.length || b.length > 65536 ||
        !letter((unsigned char)b.name[0]))
      return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0,
                      "invalid expression binding name");
    for (size_t k = 1; k < b.length; k++)
      if (!letter((unsigned char)b.name[k]) && !digit((unsigned char)b.name[k]))
        return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0,
                        "invalid expression binding name");
  }

  double* stack = rmr_calloc(p->stack_size, sizeof *stack);
  if (!stack) return rmr_fail(e, RMR_ERR_OUT_OF_MEMORY, 0, nullptr);

  size_t n = 0;
  rmr_status status = RMR_OK;
  for (size_t i = 0; i < p->count; i++) {
    instruction v = p->code[i];
    if (v.op == NUMBER)
      stack[n++] = v.number;
    else if (v.op == NAME) {
      bool found = false;
      for (size_t k = count; k > 0; k--) {
        const rmr_binding* b = &bindings[k - 1];
        if (b->length == v.length &&
            memcmp(b->name, p->source + v.offset, v.length) == 0) {
          stack[n++] = b->value;
          found = true;
          break;
        }
      }
      if (!found) {
        status = rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, v.offset,
                          "unknown expression name");
        break;
      }
    } else if (v.op == NEG)
      stack[n - 1] = -stack[n - 1];
    else if (v.op == SELECT) {
      double no = stack[--n], yes = stack[--n];
      stack[n - 1] = stack[n - 1] != 0 ? yes : no;
    } else {
      double right = stack[--n], left = stack[n - 1];
      switch (v.op) {
        case ADD:
          stack[n - 1] = left + right;
          break;
        case SUB:
          stack[n - 1] = left - right;
          break;
        case MUL:
          stack[n - 1] = left * right;
          break;
        case DIV:
          stack[n - 1] = right == 0 ? 0 : left / right;
          break;
        case LE:
          stack[n - 1] = left <= right;
          break;
        case GE:
          stack[n - 1] = left >= right;
          break;
        case EQ:
          stack[n - 1] = left == right;
          break;
        case NE:
          stack[n - 1] = left != right;
          break;
        case LT:
          stack[n - 1] = left < right;
          break;
        case GT:
          stack[n - 1] = left > right;
          break;
        case AND:
          stack[n - 1] = left != 0 && right != 0;
          break;
        case OR:
          stack[n - 1] = left != 0 || right != 0;
          break;
        default:
          break;
      }
    }
  }

  if (status == RMR_OK) *out = stack[0];

  free(stack);
  return status;
}

size_t rmr_expression_cost(const rmr_expression* p) { return p->count; }

size_t rmr_expression_stack_bytes(const rmr_expression* p) {
  return p->stack_size * sizeof(double);
}

size_t rmr_expression_bytes(const rmr_expression* p) {
  return p ? sizeof *p + strlen(p->source) + 1 +
                 p->count * sizeof(instruction) + p->stack_size * sizeof(double)
           : 0;
}
