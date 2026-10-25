// Nihad Abbasov 251ADB076
// https://github.com/NihadAbbasov12/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm

/*
 * calc - evaluates arithmetic expressions read from text files.
 *
 *   calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]
 *
 * Every non-blank line of an input file is one expression. The results are
 * written to an output file, one per line, in the same order. If any line
 * has an error, the output file holds only "ERROR:<pos>" for the first one.
 *
 * How it works:
 *   1. read_file() loads the whole input file into memory at once;
 *   2. next_token() cuts it into tokens and knows where each token starts;
 *   3. the parse_*() functions check the grammar and compute the value in
 *      the same pass (recursive descent, one function per precedence level);
 *   4. evaluate() does this for every line and collects the results in a
 *      Buffer, which is written to the output file only at the end.
 *
 * Large files: the input is read with a single fread, the tokenizer walks
 * that buffer once without copying lines out of it, comment lines are
 * skipped with memchr, and the results go into a buffer that doubles its
 * size when full. So the work grows linearly with the file size.
 */

#define _POSIX_C_SOURCE 200809L /* mkdir(), opendir() and stat() are POSIX, not ISO C */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

/* Used in the names of output files and folders. */
#define STUDENT_NAME "nihad"
#define STUDENT_LASTNAME "abbasov"
#define STUDENT_ID "251ADB076"

/* ------------------------------------------------------------------------ */
/* Output buffer                                                             */
/* ------------------------------------------------------------------------ */

/* Results are collected in memory and written only after the whole file was
 * evaluated: if an error happens on any line, the output file must contain
 * only "ERROR:<pos>", so nothing can be written before we know that. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

/* The capacity doubles when it runs out, so a file with millions of results
 * is re-allocated only about log2(n) times instead of once per result. */
static int buffer_append(Buffer *b, const char *s, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n)
            cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p)
            return 0;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Reading and writing files                                                 */
/* ------------------------------------------------------------------------ */

/* The whole file is read with one fread into one buffer of the exact size
 * (found with fseek/ftell). The tokenizer then walks this buffer once, so a
 * multi-megabyte input is never copied line by line. The extra '\0' at the
 * end guarantees that no scan can run past the data. */
static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *data = malloc((size_t)size + 1);
    if (!data) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(data);
        return NULL;
    }
    data[got] = '\0';
    *len_out = got;
    return data;
}

static int write_file(const char *path, const Buffer *b) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    /* An empty input gives an empty output file; then data is still NULL. */
    int ok = b->len == 0 || fwrite(b->data, 1, b->len, f) == b->len;
    if (fclose(f) != 0)
        ok = 0;
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Numbers                                                                   */
/* ------------------------------------------------------------------------ */

/* Every value remembers whether it is an integer or a float. A number
 * written without '.' is an integer; apply() decides the kind of each
 * result. The kind also decides the printing: integers with %lld, floats
 * with %.15g (so the float 1.25 * 4 prints as 5). */
typedef struct {
    int is_float;
    long long i; /* the value when is_float == 0 */
    double f;    /* the value when is_float == 1 */
} Value;

static Value int_value(long long i) {
    Value v = {0, i, 0.0};
    return v;
}

static Value float_value(double f) {
    Value v = {1, 0, f};
    return v;
}

static double as_double(Value v) {
    return v.is_float ? v.f : (double)v.i;
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* Converts the n characters at s (digits '.' digits) to a double. strtod()
 * gives the correctly rounded value, which adding digits up by hand would
 * not. It gets a copy that ends right after the number, so it can never
 * read further and, for example, take the "e3" of "1.5e3" as an exponent:
 * the rules say that this 'e' is an invalid character. */
static double span_to_double(const char *s, size_t n) {
    char small[64];
    char *buf = n < sizeof small ? small : malloc(n + 1);
    if (!buf)
        return 0.0;
    memcpy(buf, s, n);
    buf[n] = '\0';
    double d = strtod(buf, NULL);
    if (buf != small)
        free(buf);
    return d;
}

/* ------------------------------------------------------------------------ */
/* Tokenizer                                                                 */
/* ------------------------------------------------------------------------ */

typedef enum {
    TOK_NUMBER,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_POW,     /* "**" */
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_EOL,     /* end of the line: a '\n' or the end of the file */
    TOK_INVALID  /* a character that cannot start any token */
} TokenType;

typedef struct {
    TokenType type;
    size_t pos;  /* 1-based position of the token's first character */
    Value value; /* only for TOK_NUMBER */
} Token;

typedef struct {
    const char *src;  /* the whole input file */
    size_t len;       /* its length in bytes */
    size_t i;         /* 0-based index of the next unread character */
    Token tok;        /* the token the parser is looking at right now */
    int failed;       /* set at the first error */
    size_t error_pos; /* position of that first error */
} Parser;

/* Positions: the tokenizer keeps a single index into the whole file, never a
 * per-line one, so a token's position is just "index + 1". That is why
 * positions automatically count every character before it, including each
 * '\n' and the characters of comment lines. */
static void next_token(Parser *p) {
    while (p->i < p->len && (p->src[p->i] == ' ' || p->src[p->i] == '\t'))
        p->i++;

    Token t = {TOK_INVALID, p->i + 1, {0, 0, 0.0}};
    if (p->i >= p->len || p->src[p->i] == '\n') {
        /* The end of the line is a token as well. When an operand is missing
         * at the end of a line, the parser is looking at this token, so the
         * error lands on the '\n' (or on file length + 1 when the last line
         * has no '\n'), as the rules require. The '\n' is not consumed here:
         * the line loop in evaluate() steps over it. */
        t.type = TOK_EOL;
    } else {
        char c = p->src[p->i];
        if (is_digit(c)) {
            /* A number is digits, optionally followed by '.' and more digits.
             * The integer part is collected in an unsigned variable, which
             * wraps instead of causing undefined behaviour if a float has a
             * very long integer part (integers in the tests always fit). */
            size_t start = p->i;
            unsigned long long v = 0;
            while (p->i < p->len && is_digit(p->src[p->i])) {
                v = v * 10 + (unsigned long long)(p->src[p->i] - '0');
                p->i++;
            }
            if (p->i < p->len && p->src[p->i] == '.') {
                if (p->i + 1 < p->len && is_digit(p->src[p->i + 1])) {
                    p->i++;
                    while (p->i < p->len && is_digit(p->src[p->i]))
                        p->i++;
                    t.type = TOK_NUMBER;
                    t.value = float_value(span_to_double(p->src + start, p->i - start));
                } else {
                    /* "5." is not a number. The rules put this error on the
                     * '.', so the invalid token reported is the '.' itself. */
                    t.pos = p->i + 1;
                }
            } else {
                t.type = TOK_NUMBER;
                t.value = int_value((long long)v);
            }
        } else {
            switch (c) {
            case '+': t.type = TOK_PLUS; break;
            case '-': t.type = TOK_MINUS; break;
            case '*':
                /* "**" is one token, so in "2 ** ** 3" the second "**" is
                 * the unexpected operator, reported at its first '*'. */
                if (p->i + 1 < p->len && p->src[p->i + 1] == '*') {
                    t.type = TOK_POW;
                    p->i++;
                } else {
                    t.type = TOK_STAR;
                }
                break;
            case '/': t.type = TOK_SLASH; break;
            case '(': t.type = TOK_LPAREN; break;
            case ')': t.type = TOK_RPAREN; break;
            default: break; /* stays TOK_INVALID and is not consumed */
            }
            if (t.type != TOK_INVALID)
                p->i++;
        }
    }
    p->tok = t;
}

/* ------------------------------------------------------------------------ */
/* Parser and evaluator                                                      */
/* ------------------------------------------------------------------------ */

/* Only the first error counts, so later calls do not overwrite it. */
static void fail(Parser *p, size_t pos) {
    if (!p->failed) {
        p->failed = 1;
        p->error_pos = pos;
    }
}

/* Grammar, one function per precedence level, from lowest to highest:
 *
 *   expr    := term { ('+' | '-') term }
 *   term    := power { ('*' | '/') power }
 *   power   := primary [ '**' power ]
 *   primary := NUMBER | '(' expr ')'
 *
 * Precedence comes from the call order: parse_expr() asks parse_term() for
 * each operand, and parse_term() only returns when no more '*' or '/'
 * follows. So in 3 + 2 * 5 the term 2 * 5 is finished before the + is
 * applied, which is exactly "* before +". In the same way parse_term() gets
 * its operands from parse_power(), so ** binds tighter than * and /.
 * Parentheses go back to the top level (expr), so anything inside them is a
 * single operand for the operators around them.
 *
 * Every function evaluates while it parses, and all of them stop as soon as
 * p->failed is set, so the reported error is the first one from the left. */

static Value parse_expr(Parser *p);

static Value parse_primary(Parser *p) {
    if (p->tok.type == TOK_NUMBER) {
        Value v = p->tok.value;
        next_token(p);
        return v;
    }
    if (p->tok.type == TOK_LPAREN) {
        next_token(p);
        Value v = parse_expr(p);
        if (p->failed)
            return v;
        if (p->tok.type != TOK_RPAREN) {
            /* The inner expression stopped at something that is not ')': for a
             * missing ')' that is the end of the line, so the error is at the
             * '\n' (as required); otherwise it is the unexpected token. */
            fail(p, p->tok.pos);
            return v;
        }
        next_token(p);
        return v;
    }
    /* We expected an operand and see something else (an operator, ')', the
     * end of the line, an invalid character): report where that token starts. */
    fail(p, p->tok.pos);
    return int_value(0);
}

/* Exponentiation by squaring: about log2(exp) multiplications, so even
 * 1 ** 1000000000000 is instant. The base is squared only while more
 * exponent bits remain, so it never grows past what the result needs (the
 * tests never overflow the result itself). */
static long long int_pow(long long base, long long exp) {
    long long result = 1;
    while (exp > 0) {
        if (exp & 1)
            result *= base;
        exp >>= 1;
        if (exp > 0)
            base *= base;
    }
    return result; /* exp == 0 gives 1, including 0 ** 0 */
}

/* Computes a op b for every binary operator.
 *
 * Integer or float: if both operands are integers, the operation stays in
 * long long, so the result is exact and / truncates toward zero like C
 * (7 / 2 is 3, (0 - 7) / 2 is -3). If either operand is a float, both are
 * converted to double and the result is a float (7 / 2.0 is 3.5).
 *
 * Error positions:
 *   - division by zero: the start of the right operand (rhs_pos), because
 *     the zero is a property of that operand, e.g. '(' in 8 / (4 - 4);
 *   - an integer raised to a negative integer has no integer result: the
 *     start of the exponent (rhs_pos again), e.g. '(' in 2 ** (1 - 2);
 *   - a float result that is inf or NaN: the operator (op_pos), because
 *     both operands were fine and the operation produced it, e.g. the
 *     ** in 10.0 ** 400 or in (0 - 8) ** 0.5. */
static Value apply(Parser *p, TokenType op, Value a, Value b, size_t op_pos, size_t rhs_pos) {
    if (!a.is_float && !b.is_float) {
        switch (op) {
        case TOK_PLUS:
            return int_value(a.i + b.i);
        case TOK_MINUS:
            return int_value(a.i - b.i);
        case TOK_STAR:
            return int_value(a.i * b.i);
        case TOK_SLASH:
            if (b.i == 0) {
                fail(p, rhs_pos);
                return a;
            }
            return int_value(a.i / b.i);
        default: /* TOK_POW */
            if (b.i < 0) {
                fail(p, rhs_pos);
                return a;
            }
            return int_value(int_pow(a.i, b.i));
        }
    }

    double x = as_double(a), y = as_double(b), r;
    switch (op) {
    case TOK_PLUS:
        r = x + y;
        break;
    case TOK_MINUS:
        r = x - y;
        break;
    case TOK_STAR:
        r = x * y;
        break;
    case TOK_SLASH:
        /* 1.5 / 0.0 would give inf, but the rules call it a division by
         * zero, so the zero is checked before dividing. */
        if (y == 0.0) {
            fail(p, rhs_pos);
            return a;
        }
        r = x / y;
        break;
    default: /* TOK_POW */
        r = pow(x, y);
        break;
    }
    if (!isfinite(r)) {
        fail(p, op_pos);
        return a;
    }
    return float_value(r);
}

/* power := primary [ '**' power ]
 * ** is right-associative because, after reading "**", this function calls
 * itself for the exponent: the whole rest (3 ** 2 in 2 ** 3 ** 2) becomes
 * the exponent before anything is computed, so 2 ** 3 ** 2 = 2 ** 9 = 512.
 * The loops used for the other levels would group to the left instead. */
static Value parse_power(Parser *p) {
    Value base = parse_primary(p);
    if (p->failed || p->tok.type != TOK_POW)
        return base;
    size_t op_pos = p->tok.pos;
    next_token(p);
    size_t exp_pos = p->tok.pos; /* start of the exponent, for errors in apply() */
    Value exp = parse_power(p);
    if (p->failed)
        return base;
    return apply(p, TOK_POW, base, exp, op_pos, exp_pos);
}

/* term := power { ('*' | '/') power } */
static Value parse_term(Parser *p) {
    Value v = parse_power(p);
    while (!p->failed && (p->tok.type == TOK_STAR || p->tok.type == TOK_SLASH)) {
        TokenType op = p->tok.type;
        size_t op_pos = p->tok.pos;
        next_token(p);
        /* Remember where the right operand starts before parsing it: a
         * division by zero is reported there, even though the zero is only
         * known after the whole operand has been computed. */
        size_t rhs_pos = p->tok.pos;
        Value rhs = parse_power(p);
        if (p->failed)
            break;
        v = apply(p, op, v, rhs, op_pos, rhs_pos);
    }
    return v;
}

/* expr := term { ('+' | '-') term }
 * The loop applies each operator as soon as its right operand is read, so
 * 10 - 3 + 1 is computed as (10 - 3) + 1: + and - are left-associative
 * (the same loop makes * and / left-associative in parse_term). */
static Value parse_expr(Parser *p) {
    Value v = parse_term(p);
    while (!p->failed && (p->tok.type == TOK_PLUS || p->tok.type == TOK_MINUS)) {
        TokenType op = p->tok.type;
        size_t op_pos = p->tok.pos;
        next_token(p);
        size_t rhs_pos = p->tok.pos;
        Value rhs = parse_term(p);
        if (p->failed)
            break;
        v = apply(p, op, v, rhs, op_pos, rhs_pos);
    }
    return v;
}

/* Evaluates every line of the file and puts the text of the output file into
 * out. Returns 0 only when memory runs out. */
static int evaluate(const char *src, size_t len, Buffer *out) {
    Parser p = {.src = src, .len = len};

    while (p.i < len) {
        /* Look at the first non-blank character: '#' there makes the whole
         * line a comment. A '#' after a token is not checked here, so the
         * tokenizer reports it as an invalid character. */
        size_t j = p.i;
        while (j < len && (src[j] == ' ' || src[j] == '\t'))
            j++;
        if (j < len && src[j] == '#') {
            /* Jump straight to the next line. The comment's characters still
             * count for later positions, because positions are offsets into
             * the whole file, not into a line. */
            const char *nl = memchr(src + j, '\n', len - j);
            p.i = nl ? (size_t)(nl - src) + 1 : len;
            continue;
        }

        next_token(&p);
        if (p.tok.type != TOK_EOL) { /* an empty line gives no output */
            Value v = parse_expr(&p);
            /* A complete expression must end exactly at the end of the line. */
            if (!p.failed && p.tok.type != TOK_EOL)
                fail(&p, p.tok.pos);
            if (p.failed)
                break;
            char num[40];
            int n = v.is_float ? snprintf(num, sizeof num, "%.15g\n", v.f)
                               : snprintf(num, sizeof num, "%lld\n", v.i);
            if (!buffer_append(out, num, (size_t)n))
                return 0;
        }
        p.i++; /* step over the '\n' that ended this line */
    }

    if (p.failed) {
        out->len = 0; /* drop the results of earlier lines */
        char msg[48];
        int n = snprintf(msg, sizeof msg, "ERROR:%zu\n", p.error_pos);
        if (!buffer_append(out, msg, (size_t)n))
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Files and folders                                                         */
/* ------------------------------------------------------------------------ */

/* "labs/task1.txt" -> "task1": the input name without folders and ".txt". */
static char *input_base(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    size_t n = strlen(name);
    if (n >= 4 && strcmp(name + n - 4, ".txt") == 0)
        n -= 4;
    char *s = malloc(n + 1);
    if (!s)
        return NULL;
    memcpy(s, name, n);
    s[n] = '\0';
    return s;
}

/* "../in/" -> "in": the last folder name of a path. With -d it names the
 * default output folder. */
static char *dir_base(const char *path) {
    size_t end = strlen(path);
    while (end > 1 && path[end - 1] == '/')
        end--;
    size_t start = end;
    while (start > 0 && path[start - 1] != '/')
        start--;
    char *s = malloc(end - start + 1);
    if (!s)
        return NULL;
    memcpy(s, path + start, end - start);
    s[end - start] = '\0';
    return s;
}

/* a + b + c as a new malloc'd string, or NULL when out of memory. */
static char *concat3(const char *a, const char *b, const char *c) {
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    char *s = malloc(la + lb + lc + 1);
    if (!s)
        return NULL;
    memcpy(s, a, la);
    memcpy(s + la, b, lb);
    memcpy(s + la + lb, c, lc + 1); /* also copies the '\0' of c */
    return s;
}

/* An existing output folder is reused; a missing one is created. */
static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode);
    return mkdir(path, 0755) == 0;
}

/* Evaluates one input file and writes
 * <outdir>/<base>_<name>_<lastname>_<id>.txt. Returns 1 on success. */
static int process_file(const char *input, const char *outdir) {
    size_t len;
    char *src = read_file(input, &len);
    if (!src) {
        fprintf(stderr, "calc: cannot read input file '%s'\n", input);
        return 0;
    }

    Buffer out = {0};
    int ok = evaluate(src, len, &out);
    free(src);
    if (!ok) {
        fprintf(stderr, "calc: out of memory\n");
        free(out.data);
        return 0;
    }

    char *base = input_base(input);
    char *file = base ? concat3(base, "_" STUDENT_NAME "_" STUDENT_LASTNAME "_" STUDENT_ID, ".txt")
                      : NULL;
    char *path = file ? concat3(outdir, "/", file) : NULL;
    ok = path && write_file(path, &out);
    if (!ok)
        fprintf(stderr, "calc: cannot write output file '%s'\n", path ? path : outdir);

    free(path);
    free(file);
    free(base);
    free(out.data);
    return ok;
}

/* -d mode: evaluates every *.txt file directly inside dir. readdir() lists a
 * single folder, so subfolders are never entered, and stat() skips folders
 * whose names happen to end in ".txt". One bad file does not stop the rest,
 * but it still makes the exit code non-zero. */
static int process_dir(const char *dir, const char *outdir) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "calc: cannot open input folder '%s'\n", dir);
        return 0;
    }
    int ok = 1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 4 || strcmp(e->d_name + n - 4, ".txt") != 0)
            continue;
        char *path = concat3(dir, "/", e->d_name);
        struct stat st;
        if (!path) {
            fprintf(stderr, "calc: out of memory\n");
            ok = 0;
        } else if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && !process_file(path, outdir)) {
            ok = 0;
        }
        free(path);
    }
    closedir(d);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Command line                                                              */
/* ------------------------------------------------------------------------ */

static void usage(void) {
    fprintf(stderr,
            "usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]\n");
}

int main(int argc, char **argv) {
    const char *dir = NULL;
    const char *outdir = NULL;
    const char *input = NULL;

    for (int k = 1; k < argc; k++) {
        const char *a = argv[k];
        int is_dir = strcmp(a, "-d") == 0 || strcmp(a, "--dir") == 0;
        int is_out = strcmp(a, "-o") == 0 || strcmp(a, "--output-dir") == 0;
        if (is_dir || is_out) {
            if (k + 1 >= argc) {
                fprintf(stderr, "calc: %s needs a folder name\n", a);
                return 1;
            }
            if (is_dir)
                dir = argv[++k];
            else
                outdir = argv[++k];
        } else if (a[0] == '-') {
            fprintf(stderr, "calc: unknown option '%s'\n", a);
            usage();
            return 1;
        } else if (!input) {
            input = a;
        } else {
            fprintf(stderr, "calc: only one input file can be given\n");
            return 1;
        }
    }

    /* There must be exactly one source: a folder (-d) or a single file. */
    if ((dir != NULL) == (input != NULL)) {
        usage();
        return 1;
    }

    /* The input is checked before anything is created, so a mistyped name
     * fails with a message and does not leave an empty output folder. */
    struct stat st;
    const char *src = dir ? dir : input;
    if (stat(src, &st) != 0 || !(dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode))) {
        fprintf(stderr, "calc: input %s '%s' not found\n", dir ? "folder" : "file", src);
        return 1;
    }

    /* Without -o, results go to <input_base>_<name>_<id> in the current
     * folder, where input_base comes from the file name or, with -d, from
     * the folder name. */
    char *base = NULL;
    char *default_out = NULL;
    if (!outdir) {
        base = dir ? dir_base(dir) : input_base(input);
        default_out = base ? concat3(base, "_" STUDENT_NAME "_", STUDENT_ID) : NULL;
        if (!default_out) {
            fprintf(stderr, "calc: out of memory\n");
            free(base);
            return 1;
        }
        outdir = default_out;
    }

    int ok = ensure_dir(outdir);
    if (!ok)
        fprintf(stderr, "calc: cannot create output folder '%s'\n", outdir);
    else
        ok = dir ? process_dir(dir, outdir) : process_file(input, outdir);

    free(default_out);
    free(base);
    return ok ? 0 : 1;
}
