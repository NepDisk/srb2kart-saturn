#include "doomtype.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <assert.h>
#include "args.h"
#include "command.h"
#include "m_misc.h"


/* ------------------ */
/* Utility functions. */
/* ------------------ */


// Prints a message to the console marking it as an error.
static void err(const char* msg) {
    CONS_Alert(CONS_ERROR, "%s\n", msg);
}

// Duplicates a string, automatically allocating memory for the copy.
// Returns NULL if memory cannot be allocated for the copy.
static char* str_dup(const char* string) {
    size_t len = strlen(string) + 1;
    char *copy = malloc(len);
    return copy ? memcpy(copy, string, len) : NULL;
}


// Hashes a string using the FNV-1a algorithm.
static uint32_t str_hash(const char* string) {
    uint32_t hash = 2166136261u;
    size_t length = strlen(string);
    for (size_t i = 0; i < length; i++) {
        hash ^= (uint8_t)string[i];
        hash *= 16777619;
    }
    return hash;
}


// Attempts to parse a string as an integer value, exiting on failure.
static int try_str_to_int(const char* string) {
    char *endptr;
    errno = 0;
    long result = strtol(string, &endptr, 0);
    if (errno == ERANGE || result > INT_MAX || result < INT_MIN) {
        err(va("'%s' is out of range", string));
    }
    if (*endptr != '\0') {
        err(va("cannot parse '%s' as an integer", string));
    }
    return (int) result;
}


// Attempts to parse a string as a double value, exiting on failure.
static double try_str_to_double(const char* string) {
    char *endptr;
    errno = 0;
    double result = strtod(string, &endptr);
    if (errno == ERANGE) {
        err(va("'%s' is out of range", string));
    }
    if (*endptr != '\0') {
        err(va("cannot parse '%s' as a floating-point value", string));
    }
    return result;
}


/* --------------------------------- */
/* Vec: a dynamic array of pointers. */
/* --------------------------------- */


typedef struct {
    int count;
    int capacity;
    void** entries;
} Vec;


static Vec* vec_new() {
    Vec* vec = malloc(sizeof(Vec));
    if (!vec) {
        return NULL;
    }
    vec->count = 0;
    vec->capacity = 0;
    vec->entries = NULL;
    return vec;
}


static void vec_free(Vec* vec) {
    if (vec) {
        free(vec->entries);
        free(vec);
    }
}


static boolean vec_add(Vec* vec, void* entry) {
    if (vec->count + 1 > vec->capacity) {
        int new_capacity = vec->capacity < 8 ? 8 : vec->capacity * 2;
        void** new_array = realloc(vec->entries, sizeof(void*) * new_capacity);
        if (!new_array) {
            return false;
        }
        vec->entries = new_array;
        vec->capacity = new_capacity;
    }
    vec->entries[vec->count] = entry;
    vec->count++;
    return true;
}


/* ------------------------------------------------------------------- */
/* Map: a linear-probing hash map with string-keys and pointer-values. */
/* ------------------------------------------------------------------- */


// The map automatically grows to keep count/capacity < MAP_MAX_LOAD.
// This data structure performs optimally when it's less than half-full.
#define MAP_MAX_LOAD 0.5


typedef struct {
    char* key;
    void* value;
    uint32_t key_hash;
} MapEntry;


typedef struct {
    int count;
    int capacity;
    int max_load_threshold;
    MapEntry* entries;
} Map;


static Map* map_new() {
    Map* map = malloc(sizeof(Map));
    if (!map) {
        return NULL;
    }
    map->count = 0;
    map->capacity = 0;
    map->max_load_threshold = 0;
    map->entries = NULL;
    return map;
}


static void map_free(Map* map) {
    if (map) {
        for (int i = 0; i < map->capacity; i++) {
            MapEntry* entry = &map->entries[i];
            if (entry->key != NULL) {
                free(entry->key);
            }
        }
        free(map->entries);
        free(map);
    }
}


static MapEntry* map_find(Map* map, const char* key, uint32_t key_hash) {
    // Capacity is always a power of 2 so we can use bitwise-AND as a fast
    // modulo operator, i.e. this is equivalent to: index = key_hash % capacity.
    size_t index = key_hash & (map->capacity - 1);

    for (;;) {
        MapEntry* entry = &map->entries[index];
        if (entry->key == NULL) {
            return entry;
        } else if (key_hash == entry->key_hash && strcmp(key, entry->key) == 0) {
            return entry;
        }
        index = (index + 1) & (map->capacity - 1);
    }
}


static boolean map_grow(Map* map) {
    MapEntry* old_entries = map->entries;
    int old_capacity = map->capacity;
    int new_capacity = old_capacity < 8 ? 8 : old_capacity * 2;

    MapEntry* new_entries = malloc(sizeof(MapEntry) * new_capacity);
    if (!new_entries) {
        return false;
    }

    for (int i = 0; i < new_capacity; i++) {
        new_entries[i].key = NULL;
    }

    map->count = 0;
    map->capacity = new_capacity;
    map->max_load_threshold = new_capacity * MAP_MAX_LOAD;
    map->entries = new_entries;

    for (int i = 0; i < old_capacity; i++) {
        MapEntry* src = &old_entries[i];
        if (src->key == NULL) continue;

        MapEntry* dst = map_find(map, src->key, src->key_hash);
        dst->key = src->key;
        dst->value = src->value;
        dst->key_hash = src->key_hash;
        map->count++;
    }

    free(old_entries);
    return true;
}


// Returns true if the key was found.
static boolean map_get(Map* map, const char* key, void** value) {
    if (map->count == 0) return false;

    uint32_t key_hash = str_hash(key);
    MapEntry* entry = map_find(map, key, key_hash);
    if (entry->key == NULL) return false;

    *value = entry->value;
    return true;
}


// Adds a new entry to the map or updates the value of an existing entry.
// (Note that the map stores its own internal copy of the key string.)
static boolean map_set(Map* map, const char* key, void* value) {
    if (map->count == map->max_load_threshold) {
        if (!map_grow(map)) {
            return false;
        }
    }

    uint32_t key_hash = str_hash(key);
    MapEntry* entry = map_find(map, key, key_hash);
    if (entry->key == NULL) {
        char* key_copy = str_dup(key);
        if (!key_copy) {
            return false;
        }
        map->count++;
        entry->key = key_copy;
        entry->value = value;
        entry->key_hash = key_hash;
    } else {
        entry->value = value;
    }

    return true;
}


// Convenience wrapper for map_set(). This splits the key string into space-
// separated words and adds a separate entry to the map for each word.
static boolean map_set_splitkey(Map* map, const char* key_string, void* value) {
    char *copy_of_key_string = str_dup(key_string);
    if (!copy_of_key_string) {
        return false;
    }

    char* word_start = copy_of_key_string;
    char* string_end = word_start + strlen(copy_of_key_string);

    while (word_start < string_end) {
        if (*word_start == ' ') {
            word_start++;
            continue;
        }

        char* word_end = word_start;
        while (true) {
            if (*word_end == ' ' || *word_end == '\0') {
                break;
            }
            word_end++;
        }
        *word_end = '\0';

        if (!map_set(map, word_start, value)) {
            free(copy_of_key_string);
            return false;
        }

        word_start = word_end + 1;
    }

    free(copy_of_key_string);
    return true;
}


/* -------- */
/* Options. */
/* -------- */


typedef enum {
    OPT_FLAG,
    OPT_STR,
    OPT_INT,
    OPT_DBL,
} OptionType;


typedef union {
    char* str_val;
    int int_val;
    double dbl_val;
} OptionValue;


typedef struct {
    OptionType type;
    int count;
    int capacity;
    OptionValue* values;
    OptionValue fallback;
} Option;


static void option_free(Option* opt) {
    if (opt) {
        free(opt->values);
        free(opt);
    }
}


static boolean option_append_value(Option* opt, OptionValue value) {
    if (opt->count + 1 > opt->capacity) {
        int new_capacity = opt->capacity < 4 ? 4 : opt->capacity * 2;
        OptionValue* new_array = realloc(opt->values, sizeof(OptionValue) * new_capacity);
        if (!new_array) {
            return false;
        }
        opt->capacity = new_capacity;
        opt->values = new_array;
    }
    opt->values[opt->count] = value;
    opt->count++;
    return true;
}


static boolean option_try_set(Option* opt, char* arg) {
    if (opt->type == OPT_STR) {
        return option_append_value(opt, (OptionValue){.str_val = arg});
    }
    else if (opt->type == OPT_INT) {
        int value = try_str_to_int(arg);
        return option_append_value(opt, (OptionValue){.int_val = value});
    }
    else if (opt->type == OPT_DBL) {
        double value = try_str_to_double(arg);
        return option_append_value(opt, (OptionValue){.dbl_val = value});
    }
    assert(false);
    return false;
}


static Option* option_new() {
    Option *option = malloc(sizeof(Option));
    if (!option) {
        return NULL;
    }
    option->count = 0;
    option->capacity = 0;
    option->values = NULL;
    return option;
}


static Option* option_new_flag() {
    Option *opt = option_new();
    if (!opt) {
        return NULL;
    }
    opt->type = OPT_FLAG;
    return opt;
}


static Option* option_new_str(char* fallback) {
    Option *opt = option_new();
    if (!opt) {
        return NULL;
    }
    opt->type = OPT_STR;
    opt->fallback = (OptionValue){.str_val = fallback};
    return opt;
}


static Option* option_new_int(int fallback) {
    Option *opt = option_new();
    if (!opt) {
        return NULL;
    }
    opt->type = OPT_INT;
    opt->fallback = (OptionValue){.int_val = fallback};
    return opt;
}


static Option* option_new_double(double fallback) {
    Option *opt = option_new();
    if (!opt) {
        return NULL;
    }
    opt->type = OPT_DBL;
    opt->fallback = (OptionValue){.dbl_val = fallback};
    return opt;
}


static char* option_get_str(Option* opt) {
    if (opt->count > 0) {
        return opt->values[opt->count - 1].str_val;
    }
    return opt->fallback.str_val;
}


static int option_get_int(Option* opt) {
    if (opt->count > 0) {
        return opt->values[opt->count - 1].int_val;
    }
    return opt->fallback.int_val;
}


static double option_get_double(Option* opt) {
    if (opt->count > 0) {
        return opt->values[opt->count - 1].dbl_val;
    }
    return opt->fallback.dbl_val;
}


// Returns the option's values as a freshly-allocated array of string pointers.
static char** option_get_str_list(Option* opt) {
    if (opt->count == 0) {
        return NULL;
    }
    char** list = malloc(sizeof(char*) * opt->count);
    if (!list) {
        return NULL;
    }
    for (int i = 0; i < opt->count; i++) {
        list[i] = opt->values[i].str_val;
    }
    return list;
}


// Returns the option's values as a freshly-allocated array of integers.
static int* option_get_int_list(Option* opt) {
    if (opt->count == 0) {
        return NULL;
    }
    int* list = malloc(sizeof(int) * opt->count );
    if (!list) {
        return NULL;
    }
    for (int i = 0; i < opt->count ; i++) {
        list[i] = opt->values[i].int_val;
    }
    return list;
}


// Returns the option's values as a freshly-allocated array of doubles.
static double* option_get_double_list(Option* opt) {
    if (opt->count  == 0) {
        return NULL;
    }
    double* list = malloc(sizeof(double) * opt->count );
    if (!list) {
        return NULL;
    }
    for (int i = 0; i < opt->count ; i++) {
        list[i] = opt->values[i].dbl_val;
    }
    return list;
}


// Returns a freshly-allocated state-string for debugging.
static char* option_to_str(Option* opt) {
    if (opt->type == OPT_FLAG) {
        return va("%i", opt->count);
    }

    char *fallback = NULL;
    if (opt->type == OPT_STR) {
        fallback = str_dup(opt->fallback.str_val);
    } else if (opt->type == OPT_INT) {
        fallback = va("%i", opt->fallback.int_val);
    } else if (opt->type == OPT_DBL) {
        fallback = va("%f", opt->fallback.dbl_val);
    }

    char *values = str_dup("");
    for (int i = 0; i < opt->count; i++) {
        char *value = NULL;
        if (opt->type == OPT_STR) {
            value = str_dup(opt->values[i].str_val);
        } else if (opt->type == OPT_INT) {
            value = va("%i", opt->values[i].int_val);
        } else if (opt->type == OPT_DBL) {
            value = va("%f", opt->values[i].dbl_val);
        }
        char *old_values = values;
        if (i == 0) {
            values = str_dup(value);
        } else {
            values = va("%s, %s", old_values, value);
        }
        free(old_values);
        free(value);
    }

    char *output = va("(%s) [%s]", fallback, values);
    free(fallback);
    free(values);
    return output;
}


/* ----------------------------------------------------- */
/* ArgStream: a wrapper for an array of string pointers. */
/* ----------------------------------------------------- */


typedef struct ArgStream {
    int count;
    int index;
    char** args;
} ArgStream;


static ArgStream* argstream_new(int count, char** args) {
    ArgStream* stream = malloc(sizeof(ArgStream));
    if (!stream) {
        return NULL;
    }
    stream->count = count;
    stream->index = 0;
    stream->args = args;
    return stream;
}


static void argstream_free(ArgStream* stream) {
    free(stream);
}


static char* argstream_next(ArgStream* stream) {
    return stream->args[stream->index++];
}


static boolean argstream_has_next(ArgStream* stream) {
    return stream->index < stream->count;
}


/* ----------------- */
/* ArgParser: setup. */
/* ----------------- */


struct ArgParser {
    char* helptext;
    char* version;
    Vec* option_vec;
    Map* option_map;
    Vec* command_vec;
    Map* command_map;
    Vec* positional_args;
    ap_callback_t callback;
    char* cmd_name;
    struct ArgParser* cmd_parser;
    boolean enable_help_command;
    boolean had_memory_error;
    struct ArgParser* parent;
    boolean first_positional_arg_ends_options;
};


ArgParser* ap_new() {
    ArgParser *parser = malloc(sizeof(ArgParser));
    if (!parser) {
        return NULL;
    }

    parser->helptext = NULL;
    parser->version = NULL;
    parser->callback = NULL;
    parser->cmd_name = NULL;
    parser->cmd_parser = NULL;
    parser->enable_help_command = false;
    parser->had_memory_error = false;
    parser->parent = NULL;
    parser->first_positional_arg_ends_options = false;

    parser->option_vec = NULL;
    parser->option_map = NULL;
    parser->command_vec = NULL;
    parser->command_map = NULL;
    parser->positional_args = NULL;

    parser->option_vec = vec_new();
    if (!parser->option_vec) {
        ap_free(parser);
        return NULL;
    }

    parser->option_map = map_new();
    if (!parser->option_map) {
        ap_free(parser);
        return NULL;
    }

    parser->command_vec = vec_new();
    if (!parser->command_vec) {
        ap_free(parser);
        return NULL;
    }

    parser->command_map = map_new();
    if (!parser->command_map) {
        ap_free(parser);
        return NULL;
    }

    parser->positional_args = vec_new();
    if (!parser->positional_args) {
        ap_free(parser);
        return NULL;
    }

    return parser;
}


void ap_free(ArgParser* parser) {
    if (!parser) {
        return;
    }

    free(parser->helptext);
    free(parser->version);

    if (parser->option_map) {
        map_free(parser->option_map);
    }

    if (parser->option_vec) {
        for (int i = 0; i < parser->option_vec->count; i++) {
            option_free(parser->option_vec->entries[i]);
        }
        vec_free(parser->option_vec);
    }

    if (parser->command_map) {
        map_free(parser->command_map);
    }

    if (parser->command_vec) {
        for (int i = 0; i < parser->command_vec->count; i++) {
            ap_free(parser->command_vec->entries[i]);
        }
        vec_free(parser->command_vec);
    }

    if (parser->positional_args) {
        vec_free(parser->positional_args);
    }

    free(parser);
}


static void ap_set_memory_error_flag(ArgParser* parser) {
    parser->had_memory_error = true;

    ArgParser* parent = parser->parent;
    while (parent) {
        parent->had_memory_error = true;
        parent = parent->parent;
    }
}


void ap_set_helptext(ArgParser* parser, const char* helptext) {
    free(parser->helptext);
    parser->helptext = NULL;

    if (helptext) {
        parser->helptext = str_dup(helptext);
        if (!parser->helptext) {
            ap_set_memory_error_flag(parser);
        }
    }
}


char* ap_get_helptext(ArgParser* parser) {
    return parser->helptext;
}


void ap_set_version(ArgParser* parser, const char* version) {
    free(parser->version);
    parser->version = NULL;

    if (version) {
        parser->version = str_dup(version);
        if (!parser->version) {
            ap_set_memory_error_flag(parser);
        }
    }
}


char* ap_get_version(ArgParser* parser) {
    return parser->version;
}


void ap_first_pos_arg_ends_options(ArgParser* parser, boolean enable) {
    parser->first_positional_arg_ends_options = enable;
}


/* -------------------------------------- */
/* ArgParser: register flags and options. */
/* -------------------------------------- */


static void ap_register_option(ArgParser* parser, const char* name, Option* opt) {
    if (!opt) {
        ap_set_memory_error_flag(parser);
        return;
    }

    if (vec_add(parser->option_vec, opt)) {
        if (map_set_splitkey(parser->option_map, name, opt)) {
            return;
        } else {
            ap_set_memory_error_flag(parser);
            parser->option_vec->count--;
            option_free(opt);
            return;
        }
    } else {
        ap_set_memory_error_flag(parser);
        option_free(opt);
        return;
    }
}


// Register a new flag.
void ap_flag(ArgParser *parser, const char* name) {
    Option* opt = option_new_flag();
    ap_register_option(parser, name, opt);
}


// Register a new string-valued option.
void ap_str_opt(ArgParser* parser, const char* name, const char* fallback) {
    Option* opt = option_new_str((char*)fallback);
    ap_register_option(parser, name, opt);
}


// Register a new integer-valued option.
void ap_int_opt(ArgParser* parser, const char* name, int fallback) {
    Option* opt = option_new_int(fallback);
    ap_register_option(parser, name, opt);
}


// Register a new double-valued option.
void ap_dbl_opt(ArgParser* parser, const char* name, double fallback) {
    Option* opt = option_new_double(fallback);
    ap_register_option(parser, name, opt);
}


/* ---------------------------------- */
/* ArgParser: flag and option values. */
/* ---------------------------------- */


// Retrieve an Option instance by name.
static Option* ap_get_opt(ArgParser* parser, const char* name) {
    void* opt;
    if (!map_get(parser->option_map, name, &opt)) {
        err(va("'%s' is not a registered flag or option name", name));
    }
    return (Option*)opt;
}


// Returns the number of times the specified flag or option was found.
int ap_count(ArgParser* parser, const char* name) {
    Option* opt = ap_get_opt(parser, name);
    return opt->count;
}


// Returns true if the specified flag or option was found.
boolean ap_found(ArgParser* parser, const char* name) {
    Option* opt = ap_get_opt(parser, name);
    return opt->count > 0;
}


// Returns the value of the specified string option.
char* ap_str_value(ArgParser* parser, const char* name) {
    Option* opt = ap_get_opt(parser, name);
    return (char*)option_get_str(opt);
}


// Returns the string value at the specified index.
char* ap_str_value_at_index(ArgParser* parser, const char* name, int index) {
    Option* opt = ap_get_opt(parser, name);
    return opt->values[index].str_val;
}


// Returns the value of the specified integer option.
int ap_int_value(ArgParser* parser, const char* name) {
    Option* opt = ap_get_opt(parser, name);
    return option_get_int(opt);
}


// Returns the integer value at the specified index.
int ap_int_value_at_index(ArgParser* parser, const char* name, int index) {
    Option* opt = ap_get_opt(parser, name);
    return opt->values[index].int_val;
}


// Returns the value of the specified floating-point option.
double ap_dbl_value(ArgParser* parser, const char* name) {
    Option* opt = ap_get_opt(parser, name);
    return option_get_double(opt);
}


// Returns the floating-point value at the specified index.
double ap_dbl_value_at_index(ArgParser* parser, const char* name, int index) {
    Option* opt = ap_get_opt(parser, name);
    return opt->values[index].dbl_val;
}


// Returns an option's values as a freshly-allocated array of string pointers.
// The array's memory is not affected by calls to ap_free().
// Returns NULL if memory cannot be allocated for the array.
char** ap_str_values(ArgParser* parser, const char* name) {
    Option* opt = ap_get_opt(parser, name);
    return (char**)option_get_str_list(opt);
}


// Returns an option's values as a freshly-allocated array of integers. The
// array's memory is not affected by calls to ap_free().
// Returns NULL if memory cannot be allocated for the array.
int* ap_int_values(ArgParser* parser, const char* name) {
    Option* opt = ap_get_opt(parser, name);
    return option_get_int_list(opt);
}


// Returns an option's values as a freshly-allocated array of doubles. The
// array's memory is not affected by calls to ap_free().
// Returns NULL if memory cannot be allocated for the array.
double* ap_dbl_values(ArgParser* parser, const char* name) {
    Option* opt = ap_get_opt(parser, name);
    return option_get_double_list(opt);
}


/* -------------------------------- */
/* ArgParser: positional arguments. */
/* -------------------------------- */


// Returns true if the parser has found one or more positional arguments.
boolean ap_has_args(ArgParser* parser) {
    return parser->positional_args->count > 0;
}


// Returns the number of positional arguments.
int ap_count_args(ArgParser* parser) {
    return parser->positional_args->count;
}


// Returns the positional argument at the specified index.
char* ap_arg(ArgParser* parser, int index) {
    return (char*)parser->positional_args->entries[index];
}


// Returns the positional arguments as a freshly-allocated array of string
// pointers. The memory occupied by the returned array is not affected by
// calls to ap_free().
// Returns NULL if memory cannot be allocated for the array.
char** ap_args(ArgParser* parser) {
    int count = ap_count_args(parser);
    char** args = malloc(sizeof(char*) * count);
    if (!args) {
        return NULL;
    }
    memcpy(args, parser->positional_args->entries, sizeof(char*) * count);
    return args;
}


// Attempts to parse and return the positional arguments as a freshly
// allocated array of integers. Exits with an error message on failure. The
// memory occupied by the returned array is not affected by calls to
// ap_free().
// Returns NULL if memory cannot be allocated for the array.
int* ap_args_as_ints(ArgParser* parser) {
    int count = ap_count_args(parser);
    int* args = malloc(sizeof(int) * count);
    if (!args) {
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        *(args + i) = try_str_to_int(parser->positional_args->entries[i]);
    }
    return args;
}


// Attempts to parse and return the positional arguments as a freshly
// allocated array of doubles. Exits with an error message on failure. The
// memory occupied by the returned array is not affected by calls to
// ap_free().
// Returns NULL if memory cannot be allocated for the array.
double* ap_args_as_doubles(ArgParser* parser) {
    int count = ap_count_args(parser);
    double *args = malloc(sizeof(double) * count);
    if (!args) {
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        *(args + i) = try_str_to_double(parser->positional_args->entries[i]);
    }
    return args;
}


/* -------------------- */
/* ArgParser: commands. */
/* -------------------- */


// Register a new command.
ArgParser* ap_cmd(ArgParser* parser, const char* name) {
    ArgParser* cmd_parser = ap_new();
    if (!cmd_parser) {
        return NULL;
    }

    if (vec_add(parser->command_vec, cmd_parser)) {
        if (map_set_splitkey(parser->command_map, name, cmd_parser)) {
            parser->enable_help_command = true;
            return cmd_parser;
        } else {
            parser->command_vec--;
            ap_free(cmd_parser);
            return NULL;
        }
    } else {
        ap_free(cmd_parser);
        return NULL;
    }
}


// Register a callback function for a command.
void ap_callback(ArgParser* parser, ap_callback_t function) {
    parser->callback = function;
}


// Returns true if the parser has found a command.
boolean ap_has_cmd(ArgParser* parser) {
    return parser->cmd_name != NULL;
}


// Returns the command name, if the parser has found a command.
char* ap_cmd_name(ArgParser* parser) {
    return parser->cmd_name;
}


// Returns the command's parser instance, if the parser has found a command.
ArgParser* ap_cmd_parser(ArgParser* parser) {
    return parser->cmd_parser;
}


// Toggles support for the automatic 'help' command.
void ap_enable_help_command(ArgParser* parser, boolean enable) {
    parser->enable_help_command = enable;
}


/* --------------------------- */
/* ArgParser: parse arguments. */
/* --------------------------- */


// Parse an option of the form --name=value or -n=value.
static void ap_handle_equals_opt(ArgParser* parser, const char* prefix, const char* arg) {
    char* name = str_dup(arg);
    if (!name) {
        ap_set_memory_error_flag(parser);
        return;
    }

    *strchr(name, '=') = '\0';
    char *value = strchr(arg, '=') + 1;

    Option* option;
    boolean found = map_get(parser->option_map, name, (void**)&option);

    if (!found || option->type == OPT_FLAG) {
        err(va("%s%s is not a recognised option name", prefix, name));
    }

    if (strlen(value) == 0) {
        err(va("missing argument for %s%s", prefix, name));
    }

    if (!option_try_set(option, value)) {
        ap_set_memory_error_flag(parser);
    }

    free(name);
}

// Parse a long-form option, i.e. an option beginning with a double dash.
static void ap_handle_long_opt(ArgParser* parser, const char* arg, ArgStream* stream) {
    Option* option;
    if (map_get(parser->option_map, arg, (void**)&option)) {
        if (option->type == OPT_FLAG) {
            option->count++;
        } else if (argstream_has_next(stream)) {
            if (!option_try_set(option, argstream_next(stream))) {
                ap_set_memory_error_flag(parser);
            }
        } else {
            err(va("missing argument for --%s", arg));
        }
    } else if (strcmp(arg, "help") == 0 && parser->helptext != NULL) {
        CONS_Printf("%s\n", parser->helptext);
    } else if (strcmp(arg, "version") == 0 && parser->version != NULL) {
        CONS_Printf("%s\n", parser->version);
    } else {
        err(va("--%s is not a recognised flag or option name", arg));
    }
}


// Parse a short-form option, i.e. an option beginning with a single dash.
static void ap_handle_short_opt(ArgParser* parser, const char* arg, ArgStream* stream) {
    for (size_t i = 0; i < strlen(arg); i++) {
        char keystr[] = {arg[i], 0};
        Option* option;
        boolean found = map_get(parser->option_map, keystr, (void**)&option);
        if (!found) {
            if (arg[i] == 'h' && parser->helptext != NULL) {
                CONS_Printf("%s\n", parser->helptext);
            } else if (arg[i] == 'v' && parser->version != NULL) {
                CONS_Printf("%s\n", parser->version);
            } else if (strlen(arg) > 1) {
                err(va("'%c' in -%s is not a recognised flag or option name", arg[i], arg));
            } else {
                err(va("-%s is not a recognised flag or option name", arg));
            }
        } else if (option->type == OPT_FLAG) {
            option->count++;
        } else if (argstream_has_next(stream)) {
            if (!option_try_set(option, argstream_next(stream))) {
                ap_set_memory_error_flag(parser);
            }
        } else if (strlen(arg) > 1) {
            err(va("missing argument for '%c' in -%s", arg[i], arg));
        } else {
            err(va("missing argument for -%s", arg));
        }
    }
}


// Parse a stream of string arguments.
static void ap_parse_stream(ArgParser* parser, ArgStream* stream) {
    if (parser->had_memory_error) {
        return;
    }

    ArgParser* cmd_parser;
    boolean is_first_arg = true;

    while (argstream_has_next(stream)) {
        char* arg = argstream_next(stream);

        // If we encounter a '--' argument, turn off option-parsing.
        if (strcmp(arg, "--") == 0) {
            while (argstream_has_next(stream)) {
                if (!vec_add(parser->positional_args, argstream_next(stream))) {
                    ap_set_memory_error_flag(parser);
                }
            }
        }

        // Is the argument a long-form option or flag?
        else if (strncmp(arg, "--", 2) == 0) {
            if (strstr(arg, "=") != NULL) {
                ap_handle_equals_opt(parser, "--", arg + 2);
            } else {
                ap_handle_long_opt(parser, arg + 2, stream);
            }
        }

        // Is the argument a short-form option or flag?
        else if (arg[0] == '-') {
            if (strlen(arg) == 1 || isdigit(arg[1])) {
                if (!vec_add(parser->positional_args, arg)) {
                    ap_set_memory_error_flag(parser);
                }
            } else if (strstr(arg, "=") != NULL) {
                ap_handle_equals_opt(parser, "-", arg + 1);
            } else {
                ap_handle_short_opt(parser, arg + 1, stream);
            }
        }

        // Is the argument a registered command?
        else if (is_first_arg && map_get(parser->command_map, arg, (void**)&cmd_parser)) {
            parser->cmd_name = arg;
            parser->cmd_parser = cmd_parser;
            ap_parse_stream(cmd_parser, stream);
            if (cmd_parser->callback && !parser->had_memory_error) {
                cmd_parser->callback(arg, cmd_parser);
            }
        }

        // Is the argument the automatic 'help' command?
        else if (is_first_arg && parser->enable_help_command && strcmp(arg, "help") == 0) {
            if (argstream_has_next(stream)) {
                char* name = argstream_next(stream);
                if (map_get(parser->command_map, name, (void**)&cmd_parser)) {
                    if (cmd_parser->helptext) {
                        CONS_Printf("%s\n", cmd_parser->helptext);
                    }
                } else {
                    err(va("'%s' is not a recognised command", name));
                }
            } else {
                err(va("the help command requires an argument"));
            }
        }

        // Otherwise add the argument to our list of positionals.
        else {
            if (!vec_add(parser->positional_args, arg)) {
                ap_set_memory_error_flag(parser);
            }
            if (parser->first_positional_arg_ends_options) {
                while (argstream_has_next(stream)) {
                    if (!vec_add(parser->positional_args, argstream_next(stream))) {
                        ap_set_memory_error_flag(parser);
                    }
                }
            }
        }

        is_first_arg = false;
    }
}


// Parse an array of string arguments.
static boolean ap_parse_array(ArgParser* parser, int count, char* args[]) {
    if (parser->had_memory_error) {
        return false;
    }

    ArgStream *stream = argstream_new(count, args);
    if (!stream) {
        return false;
    }

    ap_parse_stream(parser, stream);
    argstream_free(stream);

    return !parser->had_memory_error;
}


// This function parses the application's command line arguments. It assumes that [argc] and [argv]
// are the arguments passed to main(), i.e. that the first element in [argv] is the program's name
// and should be ignored when parsing the actual *arguments*. In some situations [argv] can be
// empty, i.e. [argc == 0], which can lead to security vulnerabilities if not explicitly handled.
boolean ap_parse(ArgParser* parser, int argc, char* argv[]) {
    if (argc > 1) {
        return ap_parse_array(parser, argc - 1, argv + 1);
    }
    return !parser->had_memory_error;
}


/* --------------------- */
/* ArgParser: utilities. */
/* --------------------- */


boolean ap_had_memory_error(ArgParser* parser) {
    return parser->had_memory_error;
}


void ap_print(ArgParser* parser) {
    CONS_Printf("%s\n", "Flags/Options:");
    if (parser->option_map->count > 0) {
        for (int i = 0; i < parser->option_map->capacity; i++) {
            MapEntry* entry = &parser->option_map->entries[i];
            if (entry->key != NULL) {
                Option* opt = entry->value;
                char* opt_str = option_to_str(opt);
                CONS_Printf("%s\n", entry->key, opt_str);
                free(opt_str);
            }
        }
    } else {
        CONS_Printf("%s\n", "  [none]");
    }

    CONS_Printf("%s\n", "\nArguments:");
    if (parser->positional_args->count > 0) {
        for (int i = 0; i < parser->positional_args->count; i++) {
            CONS_Printf("%s\n", ap_arg(parser, i));
        }
    } else {
        CONS_Printf("%s\n", "  [none]");
    }

    CONS_Printf("%s\n", "\nCommand:");
    if (ap_has_cmd(parser)) {
        CONS_Printf("%s\n", ap_cmd_name(parser));
    } else {
        CONS_Printf("%s\n", "  [none]");
    }
}


/* --------------------- */
/* Deprecated functions. */
/* --------------------- */


void ap_cmd_help(ArgParser* parser, boolean enable) {
    ap_enable_help_command(parser, enable);
}


void ap_helptext(ArgParser* parser, const char* helptext) {
    ap_set_helptext(parser, helptext);
}


void ap_version(ArgParser* parser, const char* version) {
    ap_set_version(parser, version);
}
