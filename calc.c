// Nihad Abbasov 251ADB076
// https://github.com/NihadAbbasov12/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm

/*
 * calc - evaluates arithmetic expressions read from text files.
 *
 * Every non-blank line of an input file is one expression. The results are
 * written to an output file, one per line, in the same order.
 */

#define _POSIX_C_SOURCE 200809L /* mkdir() and struct stat are POSIX, not ISO C */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
/* Tokenizer                                                                 */
/* ------------------------------------------------------------------------ */

typedef enum {
    TOK_NUMBER,
    TOK_PLUS,
    TOK_MINUS,
    TOK_EOL,     /* end of the line: a '\n' or the end of the file */
    TOK_INVALID  /* a character that cannot start any token */
} TokenType;

typedef struct {
    TokenType type;
    size_t pos;      /* 1-based position of the token's first character */
    long long value; /* only for TOK_NUMBER */
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

    Token t = {TOK_INVALID, p->i + 1, 0};
    if (p->i >= p->len || p->src[p->i] == '\n') {
        /* The end of the line is a token as well. When an operand is missing
         * at the end of a line, the parser is looking at this token, so the
         * error lands on the '\n' (or on file length + 1 when the last line
         * has no '\n'), as the rules require. The '\n' is not consumed here:
         * the line loop in evaluate() steps over it. */
        t.type = TOK_EOL;
    } else {
        char c = p->src[p->i];
        if (c >= '0' && c <= '9') {
            long long v = 0;
            while (p->i < p->len && p->src[p->i] >= '0' && p->src[p->i] <= '9') {
                v = v * 10 + (p->src[p->i] - '0');
                p->i++;
            }
            t.type = TOK_NUMBER;
            t.value = v;
        } else if (c == '+') {
            t.type = TOK_PLUS;
            p->i++;
        } else if (c == '-') {
            t.type = TOK_MINUS;
            p->i++;
        }
        /* anything else stays TOK_INVALID and is not consumed */
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

/* primary := NUMBER */
static long long parse_primary(Parser *p) {
    if (p->tok.type != TOK_NUMBER) {
        /* We expected an operand and see something else (an operator, the end
         * of the line, an invalid character): report where that token starts. */
        fail(p, p->tok.pos);
        return 0;
    }
    long long v = p->tok.value;
    next_token(p);
    return v;
}

/* expr := primary { ('+' | '-') primary }
 * The loop applies each operator as soon as its right operand is read, so
 * 10 - 3 + 1 is computed as (10 - 3) + 1: + and - are left-associative. */
static long long parse_expr(Parser *p) {
    long long v = parse_primary(p);
    while (!p->failed && (p->tok.type == TOK_PLUS || p->tok.type == TOK_MINUS)) {
        TokenType op = p->tok.type;
        next_token(p);
        long long rhs = parse_primary(p);
        if (p->failed)
            return 0;
        v = (op == TOK_PLUS) ? v + rhs : v - rhs;
    }
    return v;
}

/* Evaluates every line of the file and puts the text of the output file into
 * out. Returns 0 only when memory runs out. */
static int evaluate(const char *src, size_t len, Buffer *out) {
    Parser p = {.src = src, .len = len};

    while (p.i < len) {
        next_token(&p);
        if (p.tok.type != TOK_EOL) { /* an empty line gives no output */
            long long v = parse_expr(&p);
            /* A complete expression must end exactly at the end of the line. */
            if (!p.failed && p.tok.type != TOK_EOL)
                fail(&p, p.tok.pos);
            if (p.failed)
                break;
            char num[32];
            int n = snprintf(num, sizeof num, "%lld\n", v);
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
    char *path = NULL;
    if (base) {
        size_t size = strlen(outdir) + strlen(base) +
                      sizeof("/___.txt" STUDENT_NAME STUDENT_LASTNAME STUDENT_ID);
        path = malloc(size);
        if (path)
            snprintf(path, size, "%s/%s_%s_%s_%s.txt", outdir, base, STUDENT_NAME,
                     STUDENT_LASTNAME, STUDENT_ID);
    }
    ok = path && write_file(path, &out);
    if (!ok)
        fprintf(stderr, "calc: cannot write output file in '%s'\n", outdir);

    free(path);
    free(base);
    free(out.data);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Command line                                                              */
/* ------------------------------------------------------------------------ */

static void usage(void) {
    fprintf(stderr, "usage: calc -o OUTDIR input.txt\n");
}

int main(int argc, char **argv) {
    const char *outdir = NULL;
    const char *input = NULL;

    for (int k = 1; k < argc; k++) {
        if (strcmp(argv[k], "-o") == 0 || strcmp(argv[k], "--output-dir") == 0) {
            if (k + 1 >= argc) {
                fprintf(stderr, "calc: %s needs a folder name\n", argv[k]);
                return 1;
            }
            outdir = argv[++k];
        } else if (argv[k][0] == '-') {
            fprintf(stderr, "calc: unknown option '%s'\n", argv[k]);
            usage();
            return 1;
        } else if (!input) {
            input = argv[k];
        } else {
            fprintf(stderr, "calc: only one input file can be given\n");
            return 1;
        }
    }

    if (!input || !outdir) {
        usage();
        return 1;
    }
    if (!ensure_dir(outdir)) {
        fprintf(stderr, "calc: cannot create output folder '%s'\n", outdir);
        return 1;
    }
    return process_file(input, outdir) ? 0 : 1;
}
