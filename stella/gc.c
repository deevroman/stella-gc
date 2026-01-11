#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef MAX_ALLOC_SIZE
#define MAX_ALLOC_SIZE ((size_t)1024 * 1024 * 1024 * 16)
#endif

#define SPACE_SIZE ((size_t)MAX_ALLOC_SIZE / 2)

#include "gc.h"
#include "runtime.h"
#include <stdbool.h>

#ifdef GC_DEBUG
#define GC_DEBUG_LOG(fmt, ...)                                   \
    do {                                                         \
        printf("\033[36m[DEBUG]\033[0m " fmt "\n", __VA_ARGS__); \
        fflush(stdout);                                          \
    } while (0)
#else
#define GC_DEBUG_LOG(fmt, ...)
#endif

#ifdef GC_DEBUG
#define INIT_OBJECT_HEADER(h)       \
    do {                            \
        (h)->magic = MAGIC;         \
        (h)->number = ++counter;    \
        (h)->magic_end = MAGIC_END; \
    } while (0)
#define VALIDATE_OBJECT_HEADER(h)           \
    do {                                    \
        assert((h)->magic == MAGIC);        \
        assert((h)->size < MAX_ALLOC_SIZE); \
    } while (0)

#define INIT_ROOT(rh)                        \
    do {                                     \
        (rh)->magic = ROOT_MAGIC;            \
        (rh)->root_ind = ++root_ind_counter; \
        (rh)->root_id = ++root_id_counter;   \
        (rh)->magic_end = ROOT_MAGIC_END;    \
    } while (0)
#define VALIDATE_ROOT_NODE(rh)             \
    do {                                   \
        assert((rh)->magic == ROOT_MAGIC); \
    } while (0)
#else
#define INIT_OBJECT_HEADER(h)
#define VALIDATE_OBJECT_HEADER(h)
#define INIT_ROOT(rh)
#define VALIDATE_ROOT_NODE(rh)
#endif

#ifndef GC_DEBUG
typedef struct ObjHeader {
    void* fwd;
    size_t size;
} ObjHeader;
#else
typedef struct ObjHeader {
#define MAGIC 0xEEEEEEEE
    uint32_t magic;
    uint32_t number;
    void* fwd;
    size_t size;
#define MAGIC_END 0xFFFFFFFF
    uint32_t magic_end;
} ObjHeader;
#endif

#ifndef GC_DEBUG
typedef struct RootNode {
    void** object;
    struct RootNode* next;
} RootNode;
#else
static int counter = 0;
static int root_ind_counter = 0;
static int root_id_counter = 0;

typedef struct RootNode {
#define ROOT_MAGIC 0xCCCCCCCC
#define ROOT_MAGIC_END 0xDDDDDDDD
    uint32_t magic;
    uint16_t root_ind;
    uint16_t root_id;
    void** object;
    struct RootNode* next;
    uint32_t magic_end;
} RootNode;
#endif

static RootNode* gc_roots_head = NULL;

static unsigned char* space_a = NULL;
static unsigned char* space_b = NULL;

static unsigned char* cur_start = NULL;
static unsigned char* cur_end = NULL;
static unsigned char* cur_free = NULL;   // указатель на свободное место, при перемещении объектов
static unsigned char* alloc_ptr = NULL;  // указатель на последний аллоцированный объект

static unsigned char* from_start = NULL;
static unsigned char* from_end = NULL;
static unsigned char* to_start = NULL;
static unsigned char* to_end = NULL;
static unsigned char* scan_ptr = NULL;
static unsigned char* scan_limit = NULL;

static bool collecting = false;

// stats
static size_t current_allocated_bytes = 0;
static size_t current_allocated_objects = 0;
static size_t total_allocated_bytes = 0;
static size_t total_allocated_objects = 0;
static size_t max_residency_bytes = 0;
static size_t max_residency_objects = 0;

static unsigned long read_ops = 0;
static unsigned long write_ops = 0;
static unsigned long read_barrier_triggers = 0;
static unsigned long write_barrier_triggers = 0;

static unsigned long gc_cycles = 0;

static size_t to_live_bytes = 0;
static size_t to_live_objects = 0;

static ObjHeader* obj_header_from_payload(void* payload)
{
    return payload - sizeof(ObjHeader);
}

static void* payload_from_header(ObjHeader* h)
{
    return h + 1;
}

static int ptr_in_space(void* p, unsigned char* start, unsigned char* end)
{
    return p != NULL && (unsigned char*)p >= start && (unsigned char*)p < end;
}

static int ptr_in_from_space(void* p)
{
    return collecting && ptr_in_space(p, from_start, from_end);
}

static int ptr_in_to_space(void* p)
{
    return collecting && ptr_in_space(p, to_start, to_end);
}

static void gc_start_cycle(void);
static void gc_step(size_t budget_bytes);
static void gc_finish_cycle(void);
static void* gc_forward(void* payload);
static void validateGraph(void);

static void gc_init_spaces(void)
{
    if (space_a) {
        return;
    }
#ifdef GC_DEBUG
    GC_DEBUG_LOG("%d\n", MAX_ALLOC_SIZE);
    space_a = (unsigned char*)malloc(SPACE_SIZE + sizeof(uint64_t) * 2);
    space_b = (unsigned char*)malloc(SPACE_SIZE + sizeof(uint64_t) * 2);
    ((uint64_t*)space_a)[0] = 0x4141AAAAAAAAAAAA;
    ((uint64_t*)space_a)[1] = 0x4141AAAAAAAAAAAA;
    ((uint64_t*)space_b)[0] = 0x4242BBBBBBBBBBBB;
    ((uint64_t*)space_b)[1] = 0x4242BBBBBBBBBBBB;
    space_a += sizeof(uint64_t) * 2;
    space_b += sizeof(uint64_t) * 2;
#else
    space_a = (unsigned char*)malloc(SPACE_SIZE);
    space_b = (unsigned char*)malloc(SPACE_SIZE);
#endif
    if (!space_a || !space_b) {
        fprintf(stderr, "GC: failed to allocate semi-spaces of size %zu bytes each\n", SPACE_SIZE);
        fflush(stderr);
        abort();
    }
    cur_free = cur_start = space_a;
    alloc_ptr = cur_end = space_a + SPACE_SIZE;
}

static void evacuateRoots(void)
{
#ifdef GC_DEBUG
    // print_gc_roots();
#endif
    RootNode* prev = NULL;
    for (RootNode* node = gc_roots_head; node != NULL; node = node->next) {
        GC_DEBUG_LOG("forwarding root node at %p next=%p", node, node->next);
        node = gc_forward(node);
        if (prev) {
            prev->next = node;
        } else {
            gc_roots_head = node;
        }
        prev = node;
    }
#ifdef GC_DEBUG
    // print_gc_roots();
#endif
    scan_limit = scan_ptr = cur_free;
    for (RootNode* node = gc_roots_head; node != NULL; node = node->next) {
        GC_DEBUG_LOG("forwarding child root node at %p", (void*)node);
        if (node->object && *(node->object) && ptr_in_from_space(*(node->object))) {
            *(node->object) = gc_forward(*(node->object));
        }
    }
}

static void gc_start_cycle(void)
{
    validateGraph();
    GC_DEBUG_LOG("gc_start_cycle: %zu bytes of %zu", (size_t)(alloc_ptr - cur_start), SPACE_SIZE);
    collecting = 1;
#ifdef GC_STATES
    print_gc_state();
#endif
    gc_cycles += 1;

    from_start = cur_start;
    from_end = cur_end;
    cur_free = cur_start = to_start = (cur_start == space_a) ? space_b : space_a;
    alloc_ptr = cur_end = to_end = to_start + SPACE_SIZE;
    cur_free = to_start;
    GC_DEBUG_LOG("gc_start_cycle cur_free %p", cur_free);
    scan_ptr = to_start;
    scan_limit = to_start;

    to_live_bytes = 0;
    to_live_objects = 0;

    evacuateRoots();

    scan_limit = cur_free;
    validateGraph();
}

static void* gc_forward(void* payload)
{
    if (!collecting) {
        return payload;
    }
    assert(payload != NULL);
    if (!ptr_in_from_space(payload)) {
        return payload;
    }

    ObjHeader* oh = obj_header_from_payload(payload);
    if (oh->fwd) {
        GC_DEBUG_LOG("forward: already forwarded payload=%p -> %p", payload, oh->fwd);
        return oh->fwd;
    }

    size_t sz = oh->size;
    size_t need = sizeof(ObjHeader) + sz;

    GC_DEBUG_LOG("forward: payload=%p size=%zu bytes (with header %zu); to-space used=%zu/%zu",
        payload, sz, need, (size_t)(cur_free - to_start + (cur_end - alloc_ptr)), SPACE_SIZE);

    if (cur_free + need > alloc_ptr) {
        fprintf(stderr, "GC: to-space overflow while forwarding %zu bytes to-space used=%zu/%zu.\n",
            need, (size_t)(cur_free - to_start + (cur_end - alloc_ptr)), SPACE_SIZE);
        fflush(stderr);
        exit(42);
    }

    ObjHeader* new_header = (ObjHeader*)cur_free;
    INIT_OBJECT_HEADER(new_header);
    new_header->fwd = NULL;
    new_header->size = sz;
    cur_free += need;
    scan_limit = cur_free;

    void* new_payload = payload_from_header(new_header);
    memcpy(new_payload, payload, sz);

    oh->fwd = new_payload;

    to_live_bytes += sz;
    to_live_objects += 1;

    return new_payload;
}

void* suspendPointer = NULL;

static void gc_step(size_t budget_bytes)
{
    validateGraph();
    size_t processed = 0;

    GC_DEBUG_LOG("step: start budget=%zu, scan=%zu/%zu bytes",
        budget_bytes,
        (size_t)(scan_ptr - to_start),
        (size_t)(scan_limit - to_start));

    // идём пока не просканируем всё, либо не исчерпаем лимит продвижения и копирований
    while (collecting && scan_ptr < scan_limit && processed <= budget_bytes) {
        void* target = NULL;
        // semi-dfs может прерываться. Возобновляемся если нужно
        if (suspendPointer == NULL) {
            ObjHeader* h = (ObjHeader*)scan_ptr;
            void* payload = payload_from_header(h);
            stella_object* obj = payload;
            int fields = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->object_header);
            GC_DEBUG_LOG("step: scanning object at %p, size=%zu bytes, fields=%d", payload, h->size, fields);

            for (int i = 0; i < fields; i++) {
                void* child = obj->object_fields[i];
                if (ptr_in_from_space(child)) {
                    // находим первый не перенаправленный объект
                    target = obj->object_fields[i] = gc_forward(child);
                    processed += obj_header_from_payload(target)->size;
                    break;
                }
                if (i + 1 == fields) {
                    size_t step = sizeof(ObjHeader) + h->size;
                    processed += step;
                    scan_ptr += step;
                }
            }
        } else {
            target = suspendPointer;
            suspendPointer = NULL;
        }
        // теперь идём вгрубь
        while (target != NULL) {
            if (processed > budget_bytes) {
                suspendPointer = target;
                break;
            }
            stella_object* subObj = target;
            processed += obj_header_from_payload(target)->size;
            target = NULL;
            int subFields = STELLA_OBJECT_HEADER_FIELD_COUNT(subObj->object_header);
            for (int i = 0; i < subFields; i++) {
                void* child = subObj->object_fields[i];
                if (ptr_in_from_space(child)) {
                    // запоминаем куда пойдём дальше
                    target = subObj->object_fields[i] = gc_forward(child);
                    processed += obj_header_from_payload(target)->size;
                    break; // прерываем итерации по полям, так как идём вглубь
                }
            }
        }
    }

    GC_DEBUG_LOG("step: end processed=%zu, scan=%zu/%zu bytes",
        processed,
        (size_t)(scan_ptr - to_start),
        (size_t)(scan_limit - to_start));

    if (collecting && scan_ptr >= scan_limit) {
        gc_finish_cycle();
    }
    validateGraph();
}

static void gc_finish_cycle(void)
{
    collecting = 0;
    cur_start = to_start;
    cur_end = to_end;

    GC_DEBUG_LOG("spaces flipped; new current space used (with headers): %zu bytes of %zu",
        (size_t)(alloc_ptr - cur_start), SPACE_SIZE);

    current_allocated_bytes = to_live_bytes;
    current_allocated_objects = to_live_objects;
    if (current_allocated_bytes > max_residency_bytes) {
        max_residency_bytes = current_allocated_bytes;
    }
    if (current_allocated_objects > max_residency_objects) {
        max_residency_objects = current_allocated_objects;
    }

#ifdef GC_DEBUG
    memset(from_start, 0xFF, SPACE_SIZE);
#endif
    from_start = from_end = NULL;
    to_start = to_end = scan_ptr = scan_limit = NULL;

#ifdef GC_STATES
    print_gc_state();
#endif
}

void* gc_alloc(size_t size_in_bytes)
{
#ifdef EPSILON_GC
    return malloc(size_in_bytes);
#endif
    gc_init_spaces();

    size_t need = sizeof(ObjHeader) + size_in_bytes;
    if (cur_free + need > alloc_ptr) {
        // если не может аллоцировать запускаем сборку
        if (!collecting) {
            gc_start_cycle();
        }
        if (cur_free + need > alloc_ptr) {
            // если мы уже были в процессе сборки, то имеет смысл завершить сборку
            gc_step(SIZE_MAX);
            // и запустить её снова, поскольку мусор мог уже успеть накопиться в to_space
            gc_start_cycle();
            // и только если в совсем чистом space не нашлось места, то прерываемся
            if (cur_free + need > alloc_ptr) {
                fprintf(stderr, "GC: out of to-space during allocation of %zu bytes. Memory usage: %zu/%zu bytes\n",
                    size_in_bytes, (size_t)(cur_free - to_start + (cur_end - alloc_ptr)), SPACE_SIZE);
                fflush(stderr);
                exit(42);
            }
        }
    }

    // аллоцируем новые объекты снизу в верх
    alloc_ptr -= need;
    ObjHeader* h = (ObjHeader*)alloc_ptr;
    INIT_OBJECT_HEADER(h);
    h->fwd = NULL;
    h->size = size_in_bytes;
    void* payload = payload_from_header(h);

    total_allocated_bytes += size_in_bytes;
    total_allocated_objects += 1;
    current_allocated_bytes += size_in_bytes;
    current_allocated_objects += 1;
    if (current_allocated_bytes > max_residency_bytes) {
        max_residency_bytes = current_allocated_bytes;
    }
    if (current_allocated_objects > max_residency_objects) {
        max_residency_objects = current_allocated_objects;
    }

    GC_DEBUG_LOG("alloc %zu bytes to %p (gc=0); current space used (with headers): %zu bytes of %zu",
        size_in_bytes, payload, (size_t)(alloc_ptr - cur_start), SPACE_SIZE);

    return payload;
}

void gc_read_barrier(void* object, int field_index)
{
    read_ops += 1;
#ifdef EPSILON_GC
    return;
#endif
    if (!collecting) {
        return;
    }
    assert(object != NULL);

#ifdef GC_DEBUG
    VALIDATE_OBJECT_HEADER(obj_header_from_payload(object));
#endif
    stella_object* obj = object;
    int fields = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->object_header);
    if (field_index < 0 && field_index >= fields) {
        return;
    }

    void** slot = &obj->object_fields[field_index];
    void* p = *slot;
    // перемещаем объект
    if (ptr_in_from_space(p)) {
        *slot = gc_forward(p);
        read_barrier_triggers += 1;
    }
    // и пытаемся продвинуть сборку мусора
    gc_step(0);
}

void gc_write_barrier(void* object, int field_index, void* contents)
{
    // да, барьер на запись внезапно понадобился, потому что корни честно хранятся вместе с остальными объектами
    // а это значит, что пуш корней может стриггерить сборку мусора
    // похоже компилятор Stella не ожидает такого и ожидает, что указатели на корни не перемещаемы
    // без барьера некоторые программы ломаются
    write_ops += 1;
#ifdef EPSILO_GC
    return;
#endif
#ifdef OFF_WRITE_BARRIER
    return;
#endif
    if (!collecting) {
        return;
    }
    assert(object != NULL);

    stella_object* obj = object;
    int fields = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->object_header);
    if (field_index < 0 && field_index > fields) {
        return;
    }

    void* val = contents;
    int triggered = 0;

    if (ptr_in_from_space(val)) {
        val = gc_forward(val);
        triggered = 1;
    }

    if (ptr_in_from_space(obj)) {
        void* new_obj = gc_forward(obj);
        ((stella_object*)new_obj)->object_fields[field_index] = val;
        triggered = 1;
    } else {
        obj->object_fields[field_index] = val;
    }

    if (triggered) {
        write_barrier_triggers += 1;
    }

    gc_step(sizeof(stella_object) * 2);
}

void gc_push_root(void** object)
{
#ifdef EPSILON_GC
    return;
#endif
    GC_DEBUG_LOG("gc_push_root: at address %p (points to %p) gc_roots_head=%p gc=%d", (void*)object, (object ? *object : NULL), gc_roots_head, collecting);
    RootNode* node = gc_alloc(sizeof(RootNode));
    INIT_ROOT(node);
    node->object = object;
    node->next = gc_roots_head;
    gc_roots_head = node;
}

void gc_pop_root(void** object)
{
#ifdef EPSILON_GC
    return;
#endif
    assert(gc_roots_head && "gc_pop_root: no roots to pop");
    assert(gc_roots_head->object == object && "gc_pop_root: must pop top root");

    GC_DEBUG_LOG("gc_pop_root: popping root at address %p (points to %p)", (void*)object, object ? *object : NULL);
    gc_roots_head = gc_roots_head->next;
#ifdef GC_DEBUG
    --root_ind_counter;
#endif
}

void print_gc_alloc_stats(void)
{
    printf("  Total allocated bytes: %zu\n", total_allocated_bytes);
    printf("  Total allocated objects: %zu\n", total_allocated_objects);
    printf("  Current allocated bytes: %zu\n", current_allocated_bytes);
    printf("  Current allocated objects: %zu\n", current_allocated_objects);
    printf("  Maximum residency bytes: %zu\n", max_residency_bytes);
    printf("  Maximum residency objects: %zu\n", max_residency_objects);
    printf("  Read operations: %lu\n", read_ops);
    printf("  Write operations: %lu\n", write_ops);
    printf("  Read barrier triggers: %lu\n", read_barrier_triggers);
    printf("  Write barrier triggers: %lu\n", write_barrier_triggers);
    printf("  GC cycles: %lu\n", gc_cycles);
}

void print_gc_state(void)
{
    printf("GC state:\n");
    printf("  collecting: %s\n", collecting ? "yes" : "no");
    if (!collecting) {
        size_t used = (size_t)(alloc_ptr - cur_start);
        printf("  current space used (with headers): %zu bytes of %zu\n", used, SPACE_SIZE);
    } else {
        size_t scanned = (size_t)(scan_ptr - to_start);
        size_t used_to = (size_t)(cur_free - to_start);
        printf("  from-space used (with headers): %zu bytes\n", (size_t)(from_end - from_start));
        printf("  to-space used (with headers):   %zu bytes, scanned: %zu bytes\n", used_to, scanned);
    }

    printf("  Number of roots on stack: ");
    int roots_count = 0;
    for (RootNode* r = gc_roots_head; r != NULL; r = r->next) {
        ++roots_count;
    }
    printf("%d\n", roots_count);

    printf("  Current allocated bytes (payload): %zu\n", current_allocated_bytes);
    printf("  Current allocated objects: %zu\n", current_allocated_objects);
}

void print_gc_roots(void)
{
    printf("GC Roots (stack top first):\n");
    for (RootNode* node = gc_roots_head; node != NULL; node = node->next) {
        void* val = node->object ? *(node->object) : NULL;
        printf("  root var @%p => %p.\t%p->next=%p\n", (void*)node->object, val, node, node->next);
    }
}

// Даллее методы исключительно для отладки и поиска проездов по памяти

typedef struct VisitedNode {
    void* payload;
    struct VisitedNode* next;
} VisitedNode;

static int validateChildsAlreadyChecked(VisitedNode* head, void* payload)
{
    for (VisitedNode* n = head; n != NULL; n = n->next) {
        if (n->payload == payload) {
            return 1;
        }
    }
    return 0;
}

static VisitedNode* validateChildsMark(VisitedNode* head, void* payload)
{
    VisitedNode* node = malloc(sizeof(VisitedNode));
    node->payload = payload;
    node->next = head;
    return node;
}

static void validateChildsFree(VisitedNode* head)
{
    while (head) {
        VisitedNode* next = head->next;
        free(head);
        head = next;
    }
}

static void validateChilds(void* payload, VisitedNode** visited)
{
    if (payload == NULL) {
        return;
    }
    if (validateChildsAlreadyChecked(*visited, payload)) {
        return;
    }
    *visited = validateChildsMark(*visited, payload);

    ObjHeader* h = obj_header_from_payload(payload);
    VALIDATE_OBJECT_HEADER(h);

    stella_object* obj = payload;
    int fields = STELLA_OBJECT_HEADER_FIELD_COUNT(obj->object_header);
    GC_DEBUG_LOG("step: scanning object at %p, size=%zu bytes, fields=%d", payload, h->size, fields);
    for (int i = 0; i < fields; i++) {
        void* child = obj->object_fields[i];
        if (ptr_in_from_space(child) || ptr_in_to_space(child)) {
            validateChilds(child, visited);
        }
    }
}

void validateGraph(void)
{
#ifndef GC_DEBUG
    return;
#endif
    VisitedNode* visited = NULL;

    printf("Graph validation\n");
    for (RootNode* node = gc_roots_head; node != NULL; node = node->next) {
        VALIDATE_ROOT_NODE(node);
        ObjHeader* oh = obj_header_from_payload(node);
        VALIDATE_OBJECT_HEADER(oh);
        assert(oh->fwd == NULL);
        if (ptr_in_from_space(*node->object) || ptr_in_to_space(*node->object)) {
            validateChilds(*node->object, &visited);
        }
        void* val = node->object ? *(node->object) : NULL;
        printf("  root var @%p => %p.\t%p->next=%p\n", (void*)node->object, val, node, node->next);
    }
    validateChildsFree(visited);
    printf("Graph validated\n");
}
