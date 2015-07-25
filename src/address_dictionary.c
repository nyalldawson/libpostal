#include <assert.h>
#include <dirent.h>
#include <limits.h>

#include "address_dictionary.h"

#define ADDRESS_DICTIONARY_SIGNATURE 0xBABABABA

address_dictionary_t *address_dict = NULL;

address_dictionary_t *get_address_dictionary(void) {
    return address_dict;
}

address_expansion_array *address_dictionary_get_expansions(char *key) {
    if (address_dict == NULL || address_dict->expansions == NULL) return NULL;
    khiter_t k = kh_get(str_expansions, address_dict->expansions, key);
    return k != kh_end(address_dict->expansions) ? kh_value(address_dict->expansions, k) : NULL;
}

int32_t address_dictionary_next_canonical_index(void) {
    if (address_dict == NULL || address_dict->canonical == NULL) return -1;
    return (int32_t)cstring_array_num_strings(address_dict->canonical);
}

bool address_dictionary_add_canonical(char *canonical) {
    if (address_dict == NULL || address_dict->canonical == NULL) return false;
    cstring_array_add_string(address_dict->canonical, canonical);
    return true;
}

char *address_dictionary_get_canonical(uint32_t index) {
    if (address_dict == NULL || address_dict->canonical == NULL || index > cstring_array_num_strings(address_dict->canonical)) return NULL;
    return cstring_array_get_string(address_dict->canonical, index);    
}

bool address_dictionary_add_expansion(char *key, address_expansion_t expansion) {
    int ret;

    log_debug("key=%s\n", key);
    address_expansion_array *expansions = address_dictionary_get_expansions(key);

    int32_t canonical_index;

    expansion_value_t value;
    value.value = 0;
    value.canonical = expansion.canonical_index == -1;

    bool is_prefix = false;
    bool is_suffix = false;
    bool is_phrase = false;

    for (int i = 0; i < expansion.num_dictionaries; i++) {
        dictionary_type_t dict = expansion.dictionary_ids[i];
        if (dict == DICTIONARY_CONCATENATED_SUFFIX_SEPARABLE || 
            dict == DICTIONARY_CONCATENATED_SUFFIX_INSEPARABLE) {
            is_suffix = true;
        } else if (dict == DICTIONARY_CONCATENATED_PREFIX_SEPARABLE ||
                   dict == DICTIONARY_ELISION) {
            is_prefix = true;
        } else {
            is_phrase = true;
        }
    }

    if (expansions == NULL) {
        expansions = address_expansion_array_new_size(1);
        address_expansion_array_push(expansions, expansion);
        khiter_t k = kh_put(str_expansions, address_dict->expansions, strdup(key), &ret);
        kh_value(address_dict->expansions, k) = expansions;

        value.count = 1;
        value.components = expansion.address_components;
        log_debug("value.count=%d, value.components=%d\n", value.count, value.components);

        if (is_phrase) {
            trie_add(address_dict->trie, key, value.value);
        }

        if (is_suffix) {
            trie_add_suffix(address_dict->trie, key, value.value);
        }

        if (is_prefix) {
            trie_add_prefix(address_dict->trie, key, value.value);
        }

    } else {
        uint32_t node_id = trie_get(address_dict->trie, key);
        log_debug("node_id=%d\n", node_id);
        if (node_id != NULL_NODE_ID) {
            if (!trie_get_data_at_index(address_dict->trie, node_id, &value.value)) {
                log_warn("get_data_at_index returned false\n");
                return false;
            }

            log_debug("value.count=%d, value.components=%d\n", value.count, value.components);

            if (value.count <= 0) {
                log_warn("value.count=%d\n", value.count);
            }

            value.count++;
            value.components |= expansion.address_components;

            if (!trie_set_data_at_index(address_dict->trie, node_id, value.value)) {
                log_warn("set_data_at_index returned false for node_id=%d and value=%d\n", node_id, value.value);
                return false;
            }
        }

        address_expansion_array_push(expansions, expansion);
    }

    return true;

}

static trie_prefix_result_t get_language_prefix(char *lang) {
    trie_prefix_result_t prefix = trie_get_prefix(address_dict->trie, lang);

    if (prefix.node_id == NULL_NODE_ID) {
        return NULL_PREFIX_RESULT;
    }

    prefix = trie_get_prefix_from_index(address_dict->trie, NAMESPACE_SEPARATOR_CHAR, NAMESPACE_SEPARATOR_CHAR_LEN, prefix.node_id, prefix.tail_pos);

    if (prefix.node_id == NULL_NODE_ID) {
        return NULL_PREFIX_RESULT;
    }

    return prefix;
}

phrase_array *search_address_dictionaries(char *str, char *lang) {
    if (str == NULL || lang == NULL) return NULL;

    trie_prefix_result_t prefix = get_language_prefix(lang);

    if (prefix.node_id == NULL_NODE_ID) {
        return NULL;
    }

    return trie_search_from_index(address_dict->trie, str, prefix.node_id);
}

phrase_array *search_address_dictionaries_tokens(char *str, token_array *tokens, char *lang) {
    trie_prefix_result_t prefix = get_language_prefix(lang);

    if (prefix.node_id == NULL_NODE_ID) {
        return NULL;
    }

    return trie_search_tokens_from_index(address_dict->trie, str, tokens, prefix.node_id);
}

bool address_dictionary_init(void) {
    if (address_dict != NULL) return false;

    address_dict = malloc(sizeof(address_dictionary_t));
    if (address_dict == NULL) return false;

    address_dict->canonical = cstring_array_new();

    if (address_dict->canonical == NULL) {
        goto exit_destroy_address_dict;
    }

    address_dict->expansions = kh_init(str_expansions);
    if (address_dict->expansions == NULL) {
        goto exit_destroy_address_dict;
    }

    address_dict->trie = trie_new();
    if (address_dict->trie == NULL) {
        goto exit_destroy_address_dict;
    }

    return true;

exit_destroy_address_dict:
    address_dictionary_destroy(address_dict);
    address_dict = NULL;
    return false;
}

void address_dictionary_destroy(address_dictionary_t *self) {
    if (self == NULL) return;

    if (self->canonical != NULL) {
        cstring_array_destroy(self->canonical);
    }

    if (self->expansions != NULL) {
        const char *key;
        address_expansion_array *expansions;
        kh_foreach(self->expansions, key, expansions, {
            free((char *)key);
            address_expansion_array_destroy(expansions);
        })
    }

    kh_destroy(str_expansions, self->expansions);


    if (self->trie != NULL) {
        trie_destroy(self->trie);
    }

    free(self);
}

static bool address_expansion_read(FILE *f, address_expansion_t *expansion) {
    if (f == NULL) return false;


    if (!file_read_uint32(f, (uint32_t *)&expansion->canonical_index)) {
        return false;
    }

    uint32_t language_len;

    if (!file_read_uint32(f, &language_len)) {
        return false;
    }

    if (!file_read_chars(f, expansion->language, language_len)) {
        return false;
    }

    if (!file_read_uint32(f, (uint32_t *)&expansion->num_dictionaries)) {
        return false;
    }

    for (int i = 0; i < expansion->num_dictionaries; i++) {
        if (!file_read_uint16(f, (uint16_t *)expansion->dictionary_ids + i)) {
            return false;
        }
    }

    if (!file_read_uint16(f, &expansion->address_components)) {
        return false;
    }

    return true;
}


static bool address_expansion_write(FILE *f, address_expansion_t expansion) {
    if (f == NULL) return false;

    uint32_t language_len = (uint32_t)strlen(expansion.language) + 1;

    if (!file_write_uint32(f, (uint32_t)expansion.canonical_index) ||
        !file_write_uint32(f, language_len) ||
        !file_write_chars(f, expansion.language, language_len) ||
        !file_write_uint32(f, expansion.num_dictionaries)
       ) {
        return false;
    }

    for (int i = 0; i < expansion.num_dictionaries; i++) {
        if (!file_write_uint16(f, expansion.dictionary_ids[i])) {
            return false;
        }
    }

    if (!file_write_uint16(f, expansion.address_components)) {
        return false;
    }

    return true;
}

bool address_dictionary_write(FILE *f) {
    if (address_dict == NULL || f == NULL) return false;

    if (!file_write_uint32(f, ADDRESS_DICTIONARY_SIGNATURE)) {
        return false;
    }

    uint32_t canonical_str_len = (uint32_t) cstring_array_used(address_dict->canonical);
    if (!file_write_uint32(f, canonical_str_len)) {
        return false;
    }

    if (!file_write_chars(f, address_dict->canonical->str->a, canonical_str_len)) {
        return false;
    }

    uint32_t num_keys = (uint32_t) kh_size(address_dict->expansions);

    if (!file_write_uint32(f, num_keys)) {
        return false;
    }

    const char *key;
    address_expansion_array *expansions;

    kh_foreach(address_dict->expansions, key, expansions, {
        uint32_t key_len = (uint32_t) strlen(key) + 1;
        if (!file_write_uint32(f, key_len)) {
            return false;
        }

        if (!file_write_chars(f, key, key_len)) {
            return false;
        }

        uint32_t num_expansions = expansions->n;

        if (!file_write_uint32(f, num_expansions)) {
            return false;
        }


        for (int i = 0; i < num_expansions; i++) {
            address_expansion_t expansion = expansions->a[i];
            if (!address_expansion_write(f, expansion)) {
                return false;
            }
        }
    })

    if (!trie_write(address_dict->trie, f)) {
        return false;
    }

    return true;
}

bool address_dictionary_read(FILE *f) {
    if (address_dict != NULL) return false;

    uint32_t signature;

    if (!file_read_uint32(f, &signature) || signature != ADDRESS_DICTIONARY_SIGNATURE) {
        return false;
    }

    address_dict = malloc(sizeof(address_dictionary_t));
    if (address_dict == NULL) return false;

    uint32_t canonical_str_len;

    if (!file_read_uint32(f, &canonical_str_len)) {
        goto exit_address_dict_created;
    }

    char_array *array = char_array_new_size(canonical_str_len);

    if (array == NULL) {
        goto exit_address_dict_created;
    }

    if (!file_read_chars(f, array->a, canonical_str_len)) {
        char_array_destroy(array);
        goto exit_address_dict_created;
    }

    array->n = canonical_str_len;

    address_dict->canonical = cstring_array_from_char_array(array);

    uint32_t num_keys;

    if (!file_read_uint32(f, &num_keys)) {
        return false;
    }

    address_dict->expansions = kh_init(str_expansions);

    uint32_t key_len;
    uint32_t num_expansions;
    char *key;
    address_expansion_array *expansions;

    for (uint32_t i = 0; i < num_keys; i++) {
        if (!file_read_uint32(f, &key_len)) {
            goto exit_address_dict_created;
        }

        key = malloc(key_len);
        if (key == NULL) {
            goto exit_address_dict_created;
        }

        if (!file_read_chars(f, key, key_len)) {
            free(key);
            goto exit_address_dict_created;
        }

        if (!file_read_uint32(f, &num_expansions)) {
            free(key);
            goto exit_address_dict_created;
        }

        expansions = address_expansion_array_new_size(num_expansions);
        if (expansions == NULL) {
            free(key);
            goto exit_address_dict_created;
        }

        address_expansion_t expansion;

        for (uint32_t j = 0; j < num_expansions; j++) {
            if (!address_expansion_read(f, &expansion)) {
                free(key);
                address_expansion_array_destroy(expansions);
                goto exit_address_dict_created;
            }
            address_expansion_array_push(expansions, expansion);
        }

        int ret;

        khiter_t k = kh_put(str_expansions, address_dict->expansions, key, &ret);
        kh_value(address_dict->expansions, k) = expansions;
    }

    address_dict->trie = trie_read(f);

    if (address_dict->trie == NULL) {
        goto exit_address_dict_created;
    }

    return true;

exit_address_dict_created:
    address_dictionary_destroy(address_dict);
    return false;
}


bool address_dictionary_load(char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }

    bool ret_val = address_dictionary_read(f);
    fclose(f);
    return ret_val;
}

bool address_dictionary_save(char *path) {
    if (address_dict == NULL) return false;

    FILE *f = fopen(path, "wb");

    bool ret_val = address_dictionary_write(f);
    fclose(f);
    return ret_val;
}

inline bool address_dictionary_module_setup(void) {
    return address_dictionary_load(DEFAULT_ADDRESS_EXPANSION_PATH);
}

void address_dictionary_module_teardown(void) {
    if (address_dict != NULL) {
        address_dictionary_destroy(address_dict);
    }
}
