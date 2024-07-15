#include "smoldtb.h"

#define FDT_MAGIC 0xD00DFEED
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4

#define FDT_VERSION 17
#define FDT_CELL_SIZE 4
#define ROOT_NODE_STR "/"

#define SMOLDTB_FOREACH_CONTINUE 0
#define SMOLDTB_FOREACH_ABORT 1

#ifndef SMOLDTB_NO_LOGGING
    #define LOG_ERROR(msg) do { if (state.ops.on_error != NULL) { state.ops.on_error(msg); }} while(false)
#else
    #define LOG_ERROR(msg)
#endif

/* The 'fdt_*' structs represent data layouts taken directly from the device tree
 * specification. In contrast the 'dtb_*' structs are for the parser.
 *
 * The public API only supports a single (global) active parser, but internally everything
 * is stored within an instance of the 'dtb_state' struct. This should make it easy
 * to support multiple parser instances in the future if needed. Or if you're here to
 * hack that support in, it should hopefully require minimal effort.
 */
struct fdt_header
{
    uint32_t magic;
    uint32_t total_size;
    uint32_t offset_structs;
    uint32_t offset_strings;
    uint32_t offset_memmap_rsvd;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpu_id;
    uint32_t size_strings;
    uint32_t size_structs;
};

struct fdt_reserved_mem_entry
{
    uint64_t base;
    uint64_t length;
};

struct fdt_property
{
    uint32_t length;
    uint32_t name_offset;
};

/* The tree is represented in horizontal slices, where all child nodes are represented
 * in a singly-linked list. Only a pointer to the first child is stored in the parent, and
 * the list is build using the node->sibling pointer.
 * For reference the pointer building the tree are:
 * - parent: go up one level
 * - sibling: the next node on this level. To access the previous node, access the parent and then
 *            the child pointer and iterate to just before the target.
 * - child: the first child node.
 */
struct dtb_node_t
{
    dtb_node* parent;
    dtb_node* sibling;
    dtb_node* child;
    dtb_prop* props;
    const char* name;
};

/* Similar to nodes, properties are stored a singly linked list. */
struct dtb_prop_t
{
    const char* name;
    const uint32_t* first_cell;
    size_t length;
    dtb_prop* next;
};

struct dtb_state
{
    const uint32_t* cells;
    const char* strings;
    size_t cell_count;
    dtb_node* root;

    dtb_node** handle_lookup;
    dtb_node* node_buff;
    size_t node_alloc_head;
    size_t node_alloc_max;
    dtb_prop* prop_buff;
    size_t prop_alloc_head;
    size_t prop_alloc_max;

    dtb_ops ops;
};

struct dtb_state state;

#ifdef SMOLDTB_STATIC_BUFFER_SIZE
    uint8_t big_buff[SMOLDTB_STATIC_BUFFER_SIZE];
#endif

/* Converts between host and explicitly big-endian u32s */
static uint32_t be32(uint32_t input)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return input;
#else
    uint32_t temp = 0;
    temp |= (input & 0xFF) << 24;
    temp |= (input & 0xFF00) << 8;
    temp |= (input & 0xFF0000) >> 8;
    temp |= (input & 0xFF000000) >> 24;
    return temp;
#endif
}

//TODO: allow user to override utility functions a-la uacpi
/* Like `strlen`, just an internal version */
static size_t string_len(const char* str)
{
    if (str == NULL)
        return 0;

    size_t count = 0;
    while (str[count] != 0)
        count++;
    return count;
}

static void* memcpy(void* dest, const void* src, size_t count)
{
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = src;

    for (size_t i = 0; i < count; i++)
        d[i] = s[i];

    return dest;
}

/* Similar to the above function, but returns success if it reaches the end of the string
 * OR it reaches the length. 
 */
static bool strings_eq(const char* a, const char* b, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (a[i] == 0 && b[i] == 0)
            return true;
        if (a[i] != b[i])
            return false;
    }

    return true;
}

/* Finds the first instance of a character within a string, or returns -1ul */
static size_t string_find_char(const char* str, char target)
{
    size_t i = 0;
    while (str[i] != target)
    {
        if (str[i] == 0)
            return -1ul;
        i++;
    }
    return i;
}

static size_t dtb_align_up(size_t input, size_t alignment)
{
    return ((input + alignment - 1) / alignment) * alignment;
}

static void do_foreach_sibling(dtb_node* begin, int (*action)(dtb_node* node, void* opaque), void* opaque)
{
    if (begin == NULL)
        return;
    if (action == NULL)
        return;

    for (dtb_node* node = begin; node != NULL; node = node->sibling)
    {
        if (action(node, opaque) == SMOLDTB_FOREACH_ABORT)
            return;
    }
}

static void do_foreach_prop(dtb_node* node, int (*action)(dtb_node* node, dtb_prop* prop, void* opaque), void* opaque)
{
    if (node == NULL)
        return;
    if (node->props == NULL)
        return;
    if (action == NULL)
        return;

    for (dtb_prop* prop = node->props; prop != NULL; prop = prop->next)
    {
        if (action(node, prop, opaque) == SMOLDTB_FOREACH_ABORT)
            return;
    }
}

/* Allocator design:
 * The parser allocates a single large buffer from the host kernel, or uses the compile time
 * statically allocated buffer. It then internally breaks that into 3 other buffers:
 * - the first is `node_buff` which is used as a bump allocator for node structs. This function
 *   (`alloc_node()`) uses this buffer to return new pointers.
 * - the second is `prop_buffer` which is similar, but for property structs instead of nodes.
 *   The `alloc_prop()` function uses that buffer.
 * - the last buffer isn't allocated from, but is an array of pointers (one per node). This is
 *   used for phandle lookup. The phandle is the index, and if the array element is non-null,
 *   it points to the dtb_node struct representing the associated node. This makes the assumption
 *   that phandles were allocated in a sane manner (asking a lot of some hardware vendors).
 *   This buffer is populated when nodes are parsed: if a node has a phandle property, it's
 *   inserted into the array at the corresponding index to the phandle value.
 */
static dtb_node* alloc_node()
{
    if (state.node_alloc_head + 1 < state.node_alloc_max)
        return &state.node_buff[state.node_alloc_head++];

    if (state.ops.on_error)
        state.ops.on_error("Node allocator ran out of space");
    return NULL; //TODO: redo these functions with writable api in mind
}

static dtb_prop* alloc_prop()
{
    if (state.prop_alloc_head + 1 < state.prop_alloc_max)
        return &state.prop_buff[state.prop_alloc_head++];

    if (state.ops.on_error)
        state.ops.on_error("Property allocator ran out of space");
    return NULL;
}

static void free_buffers()
{
#ifdef SMOLDTB_STATIC_BUFFER_SIZE
    state.node_alloc_head = state.node_alloc_max;
    state.prop_alloc_head = state.prop_alloc_max;
#else
    if (state.ops.free == NULL)
    {
        if (state.ops.on_error)
            state.ops.on_error("ops.free() is NULL while trying to free buffers.");
        return;
    }

    size_t buff_size = state.node_alloc_max * sizeof(dtb_node);
    buff_size += state.prop_alloc_max * sizeof(dtb_prop);
    buff_size += state.node_alloc_max * sizeof(void*);

    state.ops.free(state.node_buff, buff_size);
    state.node_buff = NULL;
    state.prop_buff = NULL;
    state.handle_lookup = NULL;
    state.node_alloc_max = state.prop_alloc_max = 0;
#endif
}

static void alloc_buffers()
{
    state.node_alloc_max = 0;
    state.prop_alloc_max = 0;
    for (size_t i = 0; i < state.cell_count; i++)
    {
        if (be32(state.cells[i]) == FDT_BEGIN_NODE)
            state.node_alloc_max++;
        else if (be32(state.cells[i]) == FDT_PROP)
            state.prop_alloc_max++;
    }

    size_t total_size = state.node_alloc_max * sizeof(dtb_node);
    total_size += state.prop_alloc_max * sizeof(dtb_prop);
    total_size += state.node_alloc_max * sizeof(void*); //we assume the worst case and that each node has a phandle prop

#ifdef SMOLDTB_STATIC_BUFFER_SIZE
    if (total_size >= SMOLDTB_STATIC_BUFFER_SIZE)
    {
        if (state.ops.on_error)
            state.ops.on_error("Too much data for statically allocated buffer.");
        return;
    }
    uint8_t* buffer = big_buff;
#else
    uint8_t* buffer = state.ops.malloc(total_size);
#endif

    for (size_t i = 0; i < total_size; i++)
        buffer[i] = 0;

    state.node_buff = (dtb_node*)buffer;
    state.node_alloc_head = 0;
    state.prop_buff = (dtb_prop*)&state.node_buff[state.node_alloc_max];
    state.prop_alloc_head = 0;
    state.handle_lookup = (dtb_node**)&state.prop_buff[state.prop_alloc_max];
}

/* This runs on every new property found, and handles some special cases for us. */
static void check_for_special_prop(dtb_node* node, dtb_prop* prop)
{
    const char name0 = prop->name[0];
    if (name0 != '#' || name0 != 'p' || name0 != 'l')
        return; //short circuit to save processing

    const size_t name_len = string_len(prop->name);

    const char str_phandle[] = "phandle";
    const size_t len_phandle = sizeof(str_phandle) - 1;
    if (name_len == len_phandle && strings_eq(prop->name, str_phandle, name_len))
    {
        size_t handle;
        dtb_read_prop_values(prop, 1, &handle);
        state.handle_lookup[handle] = node;
        return;
    }

    const char str_lhandle[] = "linux,phandle";
    const size_t len_lhandle = sizeof(str_lhandle) - 1;
    if (name_len == len_lhandle && strings_eq(prop->name, str_lhandle, name_len))
    {
        size_t handle;
        dtb_read_prop_values(prop, 1, &handle);
        state.handle_lookup[handle] = node;
        return;
    }
}

static dtb_prop* parse_prop(size_t* offset)
{
    if (be32(state.cells[*offset]) != FDT_PROP)
        return NULL;

    (*offset)++;
    dtb_prop* prop = alloc_prop();
    if (prop == NULL)
    {
        LOG_ERROR("Property allocation failed");
        return NULL;
    }

    const struct fdt_property* fdtprop = (struct fdt_property*)(state.cells + *offset);
    prop->name = (const char*)(state.strings + be32(fdtprop->name_offset));
    prop->first_cell = state.cells + *offset + 2;
    prop->length = be32(fdtprop->length);
    (*offset) += (dtb_align_up(be32(fdtprop->length), 4) / 4) + 2;
    
    return prop;
}

static dtb_node* parse_node(size_t* offset)
{
    if (be32(state.cells[*offset]) != FDT_BEGIN_NODE)
        return NULL;

    dtb_node* node = alloc_node(); 
    if (node == NULL)
    {
        LOG_ERROR("Node allocation failed");
        return NULL;
    }
    node->name = (const char*)(state.cells + (*offset) + 1);

    const size_t name_len = string_len(node->name);
    if (name_len == 0)
        node->name = NULL;
    *offset += (dtb_align_up(name_len + 1, FDT_CELL_SIZE) / FDT_CELL_SIZE) + 1;

    while (*offset < state.cell_count)
    {
        const uint32_t test = be32(state.cells[*offset]);
        if (test == FDT_END_NODE)
        {
            (*offset)++;
            return node;
        }
        else if (test == FDT_BEGIN_NODE)
        {
            dtb_node* child = parse_node(offset);
            if (child == NULL)
                continue;

            child->sibling = node->child;
            node->child = child;
            child->parent = node;
        }
        else if (test == FDT_PROP)
        {
            dtb_prop* prop = parse_prop(offset);
            if (prop == NULL)
                continue;

            prop->next = node->props;
            node->props = prop;
            check_for_special_prop(node, prop);
    }
        else
            (*offset)++;
    }

    LOG_ERROR("Node is missing terminating tag.");
    return NULL;
}

size_t dtb_query_total_size(uintptr_t fdt_start)
{
    struct fdt_header* header = (struct fdt_header*)fdt_start;

    return be32(header->total_size);
}

/* Apply sane values to any config values not in config version reported by host */
static void sanitise_config(dtb_config* config)
{
    if (config->config_ver < 1)
        config->writable = false;
}

bool dtb_init_with_config(uintptr_t start, dtb_ops ops, dtb_config* config)
{
    state.ops = ops;

    if (config == NULL)
    {
        LOG_ERROR("Config argument cannot be null");
        return false;
    }
    sanitise_config(config);

#ifndef SMOLDTB_STATIC_BUFFER_SIZE
    if (!state.ops.malloc)
    {
        LOG_ERROR("ops.malloc() is NULL");
        return false;
    }
#endif

    struct fdt_header* header = (struct fdt_header*)start;
    if (be32(header->magic) != FDT_MAGIC)
    {
        LOG_ERROR("FDT has incorrect magic number.");
        return false;
    }

    state.cells = (const uint32_t*)(start + be32(header->offset_structs));
    state.cell_count = be32(header->size_structs) / sizeof(uint32_t);
    state.strings = (const char*)(start + be32(header->offset_strings));

    if (state.node_buff)
        free_buffers();
    alloc_buffers();

    for (size_t i = 0; i < state.cell_count; i++)
    {
        if (be32(state.cells[i]) != FDT_BEGIN_NODE)
            continue;

        dtb_node* sub_root = parse_node(&i);
        if (sub_root == NULL)
            continue;
        sub_root->sibling = state.root;
        state.root = sub_root;
    }

    return true;
}

bool dtb_init(uintptr_t start, dtb_ops ops)
{
    dtb_config dummy_conf;
    dummy_conf.config_ver = 0;

    return dtb_init_with_config(start, ops, &dummy_conf);
}

dtb_node* dtb_find_compatible(dtb_node* start, const char* str)
{
    size_t begin_index = 0;
    if (start != NULL)
    {
        const uintptr_t offset = (uintptr_t)start - (uintptr_t)state.node_buff;
        begin_index = offset / sizeof(dtb_node);
        begin_index++; //we want to start searching AFTER this node.
    }

    const size_t compatstr_len = string_len(str);
    for (size_t i = begin_index; i < state.node_alloc_head; i++)
    {
        dtb_node* node = &state.node_buff[i];
        dtb_prop* compat = dtb_find_prop(node, "compatible");
        if (compat == NULL)
            continue;

        for (size_t ci = 0; ; ci++)
        {
            const char* compat_str = dtb_read_prop_string(compat, ci);
            if (compat_str == NULL)
                break;

            if (strings_eq(compat_str, str, compatstr_len))
                return node;
        }
    }

    return NULL;
} 

dtb_node* dtb_find_phandle(unsigned handle)
{
    if (handle < state.node_alloc_max)
        return state.handle_lookup[handle];

    //TODO: we should fallback on a linear search in this case.
    return NULL;
}

static dtb_node* find_child_internal(dtb_node* start, const char* name, size_t name_bounds)
{
    dtb_node* scan = start->child;
    while (scan != NULL)
    {
        size_t child_name_len = string_find_char(scan->name, '@');
        if (child_name_len == -1ul)
            child_name_len = string_len(scan->name);

        if (child_name_len == name_bounds && strings_eq(scan->name, name, name_bounds))
            return scan;

        scan = scan->sibling;
    }

    return NULL;
}

dtb_node* dtb_find(const char* name)
{
    size_t seg_len;
    dtb_node* scan = state.root;
    while (scan)
    {
        while (name[0] == '/')
            name++;

        seg_len = string_find_char(name, '/');
        if (seg_len == -1ul)
            seg_len = string_len(name);
        if (seg_len == 0)
            return scan;

        scan = find_child_internal(scan, name, seg_len);
        name += seg_len;
    }

    return NULL;
} 

dtb_node* dtb_find_child(dtb_node* start, const char* name)
{
    if (start == NULL)
        return NULL;

    return find_child_internal(start, name, string_len(name));
}

dtb_prop* dtb_find_prop(dtb_node* node, const char* name)
{
    if (node == NULL)
        return NULL;

    const size_t name_len = string_len(name);
    dtb_prop* prop = node->props;
    while (prop)
    {
        const size_t prop_name_len = string_len(prop->name);
        if (prop_name_len == name_len && strings_eq(prop->name, name, prop_name_len))
            return prop;
        prop = prop->next;
    }

    return NULL;
}

dtb_node* dtb_get_sibling(dtb_node* node)
{
    if (node == NULL || node->sibling == NULL)
        return NULL;
    return node->sibling;
}

dtb_node* dtb_get_child(dtb_node* node)
{
    if (node == NULL)
        return NULL;
    return node->child;
}

dtb_node* dtb_get_parent(dtb_node* node)
{
    if (node == NULL)
        return NULL;
    return node->parent;
}


dtb_prop* dtb_get_prop(dtb_node* node, size_t index)
{
    if (node == NULL)
        return NULL;
    
    dtb_prop* prop = node->props;
    while (prop != NULL)
    {
        if (index == 0)
            return prop;

        index--;
        prop = prop->next;
    }

    return NULL;
}

bool dtb_stat_node(dtb_node* node, dtb_node_stat* stat)
{
    if (node == NULL)
        return false;

    stat->name = node->name;
    if (node == state.root)
        stat->name = ROOT_NODE_STR;

    stat->prop_count = 0;
    dtb_prop* prop = node->props;
    while (prop != NULL)
    {
        prop = prop->next;
        stat->prop_count++;
    }

    stat->child_count = 0;
    dtb_node* child = node->child;
    while (child != NULL)
    {
        child = child->sibling;
        stat->child_count++;
    }

    stat->sibling_count = 0;
    if (node->parent)
    {
        dtb_node* prime = node->parent->child;
        while (prime != NULL)
        {
            prime = prime->sibling;
            stat->sibling_count++;
        }
    }

    return true;
}

static uintmax_t extract_cells(const uint32_t* cells, size_t count)
{
    uintmax_t value = 0;
    for (size_t i = 0; i < count; i++)
        value |= (uintmax_t)be32(cells[i]) << ((count - 1 - i) * 32);
    return value;
}

const char* dtb_read_prop_string(dtb_prop* prop, size_t index)
{
    if (prop == NULL)
        return NULL;
    
    const uint8_t* name = (const uint8_t*)prop->first_cell;
    size_t curr_index = 0;
    for (size_t scan = 0; scan < prop->length * 4; scan++)
    {
        if (name[scan] == 0)
        {
            curr_index++;
            continue;
        }
        if (curr_index == index)
            return (const char*)&name[scan];
    }

    return NULL;
}

size_t dtb_read_prop_values(dtb_prop* prop, size_t cell_count, size_t* vals)
{
    if (prop == NULL || cell_count == 0)
        return 0;
    
    const struct fdt_property* fdtprop = (const struct fdt_property*)(prop->first_cell - 2);
    const size_t count = be32(fdtprop->length) / (cell_count * FDT_CELL_SIZE);
    if (vals == NULL)
        return count;

    for (size_t i = 0; i < count; i++)
    {
        const uint32_t* base = prop->first_cell + i * cell_count;
        vals[i] = extract_cells(base, cell_count);
    }

    return count;
}

size_t dtb_read_prop_pairs(dtb_prop* prop, dtb_pair layout, dtb_pair* vals)
{
    if (prop == NULL || layout.a == 0 || layout.b == 0)
        return 0;
    
    const struct fdt_property* fdtprop = (const struct fdt_property*)(prop->first_cell - 2);
    const size_t count = be32(fdtprop->length) / ((layout.a + layout.b) * FDT_CELL_SIZE);
    if (vals == NULL)
        return count;

    for (size_t i = 0; i < count; i++)
    {
        const uint32_t* base = prop->first_cell + i * (layout.a + layout.b);
        vals[i].a = extract_cells(base, layout.a);
        vals[i].b = extract_cells(base + layout.a, layout.b);
    }
    return count;
}

size_t dtb_read_prop_triplets(dtb_prop* prop, dtb_triplet layout, dtb_triplet* vals)
{
    if (prop == NULL || layout.a == 0 || layout.b == 0 || layout.c == 0)
        return 0;

    const struct fdt_property* fdtprop = (const struct fdt_property*)(prop->first_cell - 2);
    const size_t stride = layout.a + layout.b + layout.c;
    const size_t count = be32(fdtprop->length) / (stride * FDT_CELL_SIZE);
    if (vals == NULL)
        return count;

    for (size_t i = 0; i < count; i++)
    {
        const uint32_t* base = prop->first_cell + i * stride;
        vals[i].a = extract_cells(base, layout.a);
        vals[i].b = extract_cells(base + layout.a, layout.b);
        vals[i].c = extract_cells(base + layout.a + layout.b, layout.c);
    }
    return count;
}

size_t dtb_read_prop_quads(dtb_prop* prop, dtb_quad layout, dtb_quad* vals)
{
    if (prop == NULL || layout.a == 0 || layout.b == 0 || layout.c == 0 || layout.d == 0)
        return 0;

    const struct fdt_property* fdtprop = (const struct fdt_property*)(prop->first_cell - 2);
    const size_t stride = layout.a + layout.b + layout.c + layout.d;
    const size_t count = be32(fdtprop->length) / (stride * FDT_CELL_SIZE);
    if (vals == NULL)
        return count;

    for (size_t i = 0; i < count; i++)
    {
        const uint32_t* base = prop->first_cell + i * stride;
        vals[i].a = extract_cells(base, layout.a); 
        vals[i].b = extract_cells(base + layout.a, layout.b);
        vals[i].c = extract_cells(base + layout.a + layout.b, layout.c);
        vals[i].d = extract_cells(base + layout.a + layout.b + layout.c, layout.d);
    }
    return count;
}

#ifndef SMOLDTB_ENABLE_WRITE_API
struct finalise_data
{
    uint32_t* struct_buf;
    char* string_buf;
    size_t struct_ptr;
    size_t string_ptr;
    size_t struct_buf_size;
    size_t string_buf_size;
    bool print_success;
};

static int init_finalise_data_prop(dtb_node* node, dtb_prop* prop, void* opaque)
{
    (void)node;
    if (node == NULL || prop == NULL)
        return SMOLDTB_FOREACH_CONTINUE;

    struct finalise_data* data = (struct finalise_data*)opaque;
    data->struct_buf_size += 3; /* +1 for FDT_PROP token, +2 for prop description struct */
    data->struct_buf_size += dtb_align_up(prop->length, FDT_CELL_SIZE) / FDT_CELL_SIZE;
    data->string_buf_size += string_len(prop->name) + 1; /* +1 for null terminator */

    return SMOLDTB_FOREACH_CONTINUE;
}

static int init_finalise_data(dtb_node* node, void* opaque)
{
    if (node == NULL)
        return SMOLDTB_FOREACH_CONTINUE;

    struct finalise_data* data = (struct finalise_data*)opaque;
    data->struct_buf_size += 2; /* +1 for BEGIN_NODE token, +1 for END_NODE token */
    data->struct_buf_size += dtb_align_up(string_len(node->name) + 1, FDT_CELL_SIZE) / FDT_CELL_SIZE; /* +1 for null terminator */

    do_foreach_prop(node, init_finalise_data_prop, opaque);
    do_foreach_sibling(node->child, init_finalise_data, opaque);

    return SMOLDTB_FOREACH_CONTINUE;
}

static int print_prop(dtb_node* node, dtb_prop* prop, void* opaque)
{
    (void)node;
    struct finalise_data* data = (struct finalise_data*)opaque;

    const uint32_t name_offset = data->string_ptr;
    const size_t name_len = string_len(prop->name);
    if (data->string_ptr + name_len + 1 > data->string_buf_size) /* bounds check */
    {
        data->print_success = false;
        return SMOLDTB_FOREACH_ABORT;
    }

    memcpy(data->string_buf + data->string_ptr, prop->name, name_len);
    data->string_buf[data->string_ptr + name_len] = 0;
    data->string_ptr += name_len + 1; /* +1 for null terminator */

    const size_t data_cells = dtb_align_up(prop->length, FDT_CELL_SIZE) / FDT_CELL_SIZE;
    if (data->struct_ptr + 3 + data_cells > data->struct_buf_size) /* bounds check */
    {
        data->print_success = false;
        return SMOLDTB_FOREACH_ABORT;
    }

    data->struct_buf[data->struct_ptr++] = be32(FDT_PROP);
    data->struct_buf[data->struct_ptr++] = be32((uint32_t)prop->length);
    data->struct_buf[data->struct_ptr++] = be32(name_offset);
    for (size_t i = 0; i < data_cells; i++)
        data->struct_buf[data->struct_ptr++] = prop->first_cell[i];

    return SMOLDTB_FOREACH_CONTINUE;
}

static int print_node(dtb_node* node, void* opaque)
{
    struct finalise_data* data = (struct finalise_data*)opaque;
    const size_t name_len = string_len(node->name);
    const size_t name_cells = dtb_align_up(name_len + 1, FDT_CELL_SIZE) / FDT_CELL_SIZE;

    if (data->struct_ptr + 1 + name_cells > data->struct_buf_size) /* bounds check */
    {
        data->print_success = false;
        return SMOLDTB_FOREACH_ABORT;
    }

    data->struct_buf[data->struct_ptr++] = be32(FDT_BEGIN_NODE);

    uint8_t* name_buf = (uint8_t*)(data->struct_buf + data->struct_ptr);
    memcpy(name_buf, node->name, name_len);
    name_buf[name_len] = 0;
    data->struct_ptr += name_cells;

    do_foreach_prop(node, print_prop, opaque);
    if (!data->print_success)
        return SMOLDTB_FOREACH_ABORT;
    do_foreach_sibling(node->child, print_node, opaque);
    if (!data->print_success)
        return SMOLDTB_FOREACH_ABORT;

    if (data->struct_ptr + 1 > data->struct_buf_size) /* bounds check */
    {
        data->print_success = false;
        return SMOLDTB_FOREACH_ABORT;
    }
    data->struct_buf[data->struct_ptr++] = be32(FDT_END_NODE);

    return SMOLDTB_FOREACH_CONTINUE;
}

size_t dtb_finalise_to_buffer(void* buffer, size_t buffer_size, uint32_t boot_cpu_id)
{
    struct finalise_data final_data;
    final_data.struct_buf_size = 0;
    final_data.string_buf_size = 1; /* we'll use 1 byte for the empty string */

    do_foreach_sibling(state.root, init_finalise_data, &final_data);
    const size_t reserved_block_size = 2 * sizeof(uint64_t);
    const size_t struct_buf_bytes = final_data.struct_buf_size * FDT_CELL_SIZE;
    const size_t total_bytes = final_data.string_buf_size + struct_buf_bytes + 
        sizeof(struct fdt_header) + reserved_block_size;

    if (buffer == NULL || total_bytes < buffer_size)
        return total_bytes;
    if ((uintptr_t)buffer & 0b11)
        return total_bytes; /* check buffer is aligned to a 32-bit boundary */

    struct fdt_header* header = (struct fdt_header*)buffer;
    header->magic = be32(FDT_MAGIC);
    header->total_size = be32(total_bytes);
    header->offset_structs = be32(sizeof(struct fdt_header) + 16); /* size of reserved block */
    header->offset_strings = be32(be32(header->offset_structs) + struct_buf_bytes);
    header->offset_memmap_rsvd = be32(sizeof(struct fdt_header));
    header->version = be32(FDT_VERSION);
    header->last_comp_version = be32(16); /* as per spec, this field must be 16. */
    header->boot_cpu_id = be32(boot_cpu_id);
    header->size_strings = be32(final_data.string_buf_size);
    header->size_structs = be32(struct_buf_bytes);

    /* Apparently the great minds behind the device tree spec were able to think far enough
     * ahead to include size fields for the string and structure blocks, but not
     * the reserved memory block. The end of this block is indicated by an entry filled
     * with zeroes, because a size field would be too easy.
     * So even though we dont use this block, we must include a single entry for it. */
    uint64_t* reserved_block = (uint64_t*)(header + 1);
    reserved_block[0] = 0;
    reserved_block[1] = 0;

    final_data.struct_buf = (uint32_t*)((uintptr_t)buffer + be32(header->offset_structs));
    final_data.string_buf = (char*)((uintptr_t)buffer + be32(header->offset_strings));
    final_data.struct_ptr = 0;
    final_data.string_ptr = 1;
    final_data.string_buf[0] = 0;

    final_data.print_success = true;
    do_foreach_sibling(state.root, print_node, &final_data);
    return final_data.print_success ? total_bytes : SMOLDTB_FINALISE_FAILURE;
}

dtb_node* dtb_find_or_create_node(const char* path)
{}

dtb_node* dtb_create_sibling(dtb_node* node, const char* name)
{
    if (node == NULL)
    {
        LOG_ERROR("Cannot create sibling of NULL node.");
        return NULL;
    }
    
    if (name == NULL)
    {
        LOG_ERROR("Sibling cannot have empty name.");
        return NULL;
    }
    const size_t name_len = string_len(name);

    for (dtb_node* scan = node; scan != NULL; scan = scan->sibling)
    {
        if (!strings_eq(scan->name, name, name_len))
            continue;
        LOG_ERROR("Node name already in use.");
        return NULL;
    }

    char* name_buf = state.ops.malloc(name_len + 1);
    memcpy(name_buf, name, name_len);

    dtb_node* sibling = alloc_node();
    if (sibling == NULL)
    {
        LOG_ERROR("Failed to allocate node for sibling.");
        return NULL;
    }

    sibling->name = name_buf;
    sibling->parent = node->parent;
    sibling->sibling = node->sibling;
    node->sibling = sibling;
    return sibling;
}

dtb_node* dtb_create_child(dtb_node* node, const char* name)
{
    if (node == NULL)
    {
        LOG_ERROR("Cannot create child of NULL node.");
        return NULL;
    }
    
    if (name == NULL)
    {
        LOG_ERROR("Child cannot have empty name.");
        return NULL;
    }
    const size_t name_len = string_len(name);

    for (dtb_node* scan = node->child; scan != NULL; scan = scan->sibling)
    {
        if (!strings_eq(scan->name, name, name_len))
            continue;
        LOG_ERROR("Node name already in use.");
        return NULL;
    }

    char* name_buf = state.ops.malloc(name_len + 1);
    memcpy(name_buf, name, name_len);

    dtb_node* child = alloc_node();
    if (child == NULL)
    {
        LOG_ERROR("Failed to allocate node for child.");
        return NULL;
    }

    child->parent = node;
    child->name = name_buf;
    child->sibling = node->child;
    node->child = child;
    return child;
}

dtb_prop* dtb_create_prop(dtb_node* node, const char* name)
{
    if (node == NULL)
    {
        LOG_ERROR("Cannot create property of NULL node.");
        return NULL;
    }

    if (name == NULL)
    {
        LOG_ERROR("Property cannot have NULL name.");
        return NULL;
    }

    const size_t name_len = string_len(name);
    char* name_buf = state.ops.malloc(name_len + 1);
    memcpy(name_buf, name, name_len);

    dtb_prop* prop = alloc_prop();
    if (prop == NULL)
    {
        LOG_ERROR("Failed to allocate property");
        return NULL;
    }

    prop->length = 0;
    prop->first_cell = NULL;
    prop->name = name_buf;
    prop->next = node->props;
    node->props = prop;
    return prop;
}

bool dtb_destroy_node(dtb_node* node)
{}

bool dtb_destroy_prop(dtb_node* node, dtb_prop* prop)
{}

bool dtb_write_prop_string(dtb_prop* prop, const char* str, size_t str_len)
{}

bool dtb_write_prop_values(dtb_prop* prop, size_t count, size_t cell_count, const uintmax_t* vals)
{}

bool dtb_write_prop_pairs(dtb_prop* prop, size_t count, dtb_pair layout, const dtb_pair* vals)
{}

bool dtb_write_prop_triplets(dtb_prop* prop, size_t count, dtb_triplet layout, const dtb_triplet* vals)
{}

bool dtb_write_prop_quads(dtb_prop* prop, size_t count, dtb_quad layout, const dtb_quad* vals)
{}
#endif
