#ifndef CFGGEN_H
#define CFGGEN_H

#include "uthash.h"
#include "utlist.h"
#include "graph.h"
#include "stringpool.h"

enum Vertex_Type {
  RETURN,          /* return instruction */
  CALL,            /* indirect call */
  FUNCTION,        /* function */
  RETURNADDR       /* return address */
};

enum ICF_Type {
  VirtualMethodCall,
  PointerToMethodCall,
  NormalCall
};

enum Qualifiers {
  CONSTANT = 1,
  VOLATILE = 2,
  STATIC = 4,
  VIRTUAL = 8
};

typedef struct code_module_t code_module;
typedef struct function_t function;
typedef struct icf_t icf;
typedef struct sym_t sym;

struct icf_t {
  char *id;
  enum ICF_Type ity;
  char *type;
  unsigned char attrs; /* constant / volatile */
  char *class_name;
  char *method_name;
  size_t offset;
  UT_hash_handle hh;
};

static icf *alloc_icf(void) {
  icf *i = malloc(sizeof(*i));
  if (!i) oom();
  return i;
}

static void free_icf(icf *i) {
  free(i);
}

struct function_t {
  char *name;
  char *class_name;
  char *method_name;
  char *type;           /* type of the function */
  size_t offset;        /* offset relative to the code start */
  node *returns;        /* return instructions */
  node *dtails;         /* direct tail calls */
  node *itails;         /* indirect tail calls */
  struct function_t *prev, *next;
};

static function *alloc_function(void) {
  function *f = malloc(sizeof(*f));
  if (!f) oom();
  memset(f, 0, sizeof(*f));
  return f;
}

static void free_function(function *f) {
  node *elt, *tmp;
  DL_FOREACH_SAFE(f->returns, elt, tmp) {
    DL_DELETE(f->returns, elt);
    free(elt);
  }
  DL_FOREACH_SAFE(f->dtails, elt, tmp) {
    DL_DELETE(f->dtails, elt);
    free(elt);
  }
  DL_FOREACH_SAFE(f->itails, elt, tmp) {
    DL_DELETE(f->itails, elt);
    free(elt);
  }
  free(f);
}

typedef struct symbol_t {
  char *name;           /* return address of a direct call */
  size_t offset;
  struct symbol_t *next, *prev;
} symbol;

static symbol *alloc_sym(void) {
  symbol *r = malloc(sizeof(*r));
  if (!r) oom();
  memset(r, 0, sizeof(*r));
  return r;
};

/**
 * A code module may be an executable or a *.so library.
 */
struct code_module_t {
  struct code_module_t *next, *prev;
  uintptr_t base_addr; /* base addr */
  icf      *icfs;      /* indirect call instructions */
  function *functions; /* functions */
  dict     *classes;   /* classes */  
  dict     *cha;       /* class hierarchy */
  graph    *aliases;   /* aliases */
  symbol   *rai;       /* return addresses of indirect calls */
  symbol   *rad;       /* return addresses of direct calls */
  symbol   *funcsyms;  /* function symbols */
  symbol   *icfsyms;   /* indirect branch symbols */
  vertex   *fats;      /* functions whose addresses are taken */
  int      cfggened;   /* the cfg has been generated for this module before */
};

static code_module *alloc_code_module(void) {
  code_module *cm = malloc(sizeof(*cm));
  if (!cm) oom();
  memset(cm, 0, sizeof(*cm));
  return cm;
}

/**
 * In a char stream pointed to by "cursor", find the next symbol "sym".
 * If a NULL-byte ('\0') is encountered, set stop to be TRUE. Return
 * the distance from cursor to the symbol.
 */
static size_t next_symbol(char *cursor, char sym, /*out*/int *stop) {
  size_t advanced = 0;

  if (*cursor) {
    while (*cursor != sym && *cursor != '\0') {
      cursor++;
      advanced++;
    }
  }

  *stop = (*cursor == '\0');

  return advanced;
}

static void print_string(const void *str) {
  dprintf(STDERR_FILENO, "%s", (const char*)str);
}

static void cha_cc_print(graph *g) {
  dict_print(g, print_string, print_cc);
}

static char *_get_string_before_symbol(/*in/out*/char **cursor, char symbol,
                                       /*out*/int *stop, /*out*/size_t *len) {
  size_t name_len = next_symbol(*cursor, symbol, stop);

  char *string = *cursor;

  *cursor += name_len;

  **cursor = '\0'; /* patch the symbol so that 'string' is a valid string */

  *cursor += 1;

  if (len) *len = name_len;
  
  return string;
}

/**
 * Intern the string in the string pool, and return the interned string.
 */
static char *sp_intern_string(str **sp, char *string) {
  return sp_add_cpy_or_nothing(sp, string);
}

/* Note that parse_* functions are not reentrant, because the first time
 * they are invoked, the contents are changed */

static char *parse_inheritance(char *cursor, const char* end,
                               /*out*/graph **cha, /*out*/str **sp) {
  int stop;

  char *sub_class_name =
    sp_intern_string(sp, _get_string_before_symbol(&cursor, '#', &stop, 0));

  while (!stop) {
    char *super_class_name =
      sp_intern_string(sp, _get_string_before_symbol(&cursor, '#', &stop, 0));

    g_add_edge(cha, sub_class_name, super_class_name);
  }

  return cursor;
}

static unsigned char _get_cpp_method_attr(/*in/out*/char *method_name,
                                          size_t len) {
  unsigned char attrs = 0;

  int counter = 5; /* only cosv qualifiers are possible */

  for (--len; len > 0 && method_name[len] != '@' && counter > 0;
       --len, --counter) {
    switch (method_name[len]) {
    case 'c': attrs |= CONSTANT; break;
    case 'o': attrs |= VOLATILE; break;
    case 's': attrs |= STATIC;   break;
    case 'v': attrs |= VIRTUAL;  break;
    }
  }

  /* patch the byte so that we can later manipulate the name without any copy */
  if (method_name[len] == '@') {
    method_name[len] = '\0';
    /* only if there really are type qualifiers can we return the attrs*/
    return attrs;
  }
  return 0;
}

static int is_constant(unsigned char attrs) { return attrs & CONSTANT; }
static int is_volatile(unsigned char attrs) { return attrs & VOLATILE; }
static int is_static(unsigned char attrs)   { return attrs & STATIC;   }
static int is_virtual(unsigned char attrs)  { return attrs & VIRTUAL;  }

static char *parse_classes(char *cursor, const char *end,
                           /*out*/dict **classes, /*out*/str **sp) {
  int stop;

  char *class_name =
    sp_intern_string(sp, _get_string_before_symbol(&cursor, '#', &stop, 0));
  //dprintf(STDERR_FILENO, "M@ %s\n", class_name);
  keyvalue *class_entry = dict_find(*classes, class_name);

  if (!class_entry)
    class_entry = dict_add(classes, class_name, 0);

  while (!stop) {
    size_t len;

    char *method_name = _get_string_before_symbol(&cursor, '#', &stop, &len);

    unsigned char attrs = _get_cpp_method_attr(method_name, len);

    method_name = sp_intern_string(sp, method_name);

    keyvalue *method_entry = dict_find((dict*)class_entry->value, method_name);

    if (!method_entry) {
      dict_add((dict**)&(class_entry->value), method_name,
               (void*)(unsigned long)attrs); /* suppress compiler warning */
    } else {
      if ((unsigned char)method_entry->value != attrs) {
        dprintf(STDERR_FILENO, "Duplicated method name %s in class %s\n",
                method_name, class_name);
        quit(-1);
      }
    }
  }

  return cursor;
}

static void parse_cha(char* content, const char *end,
                      /*out*/dict **classes, /*out*/graph **cha, /*out*/str **sp) {
  char *cursor = content;

  while (cursor < end) {
    switch (*cursor) {
    case 'I':
      cursor = parse_inheritance(cursor+2, end, cha, sp);
      break;
    case 'M':
      cursor = parse_classes(cursor+2, end, classes, sp);
      break;
    default:
      dprintf(STDERR_FILENO, "Invalid cha info at offset %ld, %s\n", cursor - content, cursor);
      quit(-1);
    }
  }
}

/**
 * parse indirect control-flow transfer metadata.
 */
static void parse_icfs(char *content, const char *end, /*out*/icf **icfs,
                       /*out*/str **sp) {
  char *cursor = content;
  
  while (cursor < end) {
    int stop;

    char *id = _get_string_before_symbol(&cursor, '#', &stop, 0);

    /* TODO: replace the following buggy code with a decent solution.
    if (0 != sp_str_handle(sp, id)) {
      // if parse_functions is invoked first, it is possible that the id has been added!!!
      dprintf(STDERR_FILENO, "Non-unique id, %s, %s\n", id, sp_str_handle(sp, id));
      quit(-1);
    }
    */
    id = sp_intern_string(sp, id);

    icf *ic = alloc_icf();

    ic->id = id;

    ic->offset = 0;

    HASH_ADD_PTR(*icfs, id, ic);

    cursor += 2;

    switch (*(cursor-2)) {
    case 'V':
      {
        char *class_name =
          sp_intern_string(sp, _get_string_before_symbol(&cursor, '#', &stop, 0));

        char *method_name =
          sp_intern_string(sp, _get_string_before_symbol(&cursor, '#', &stop, 0));

        ic->ity = VirtualMethodCall;
        ic->class_name = class_name;
        ic->method_name = method_name;
      }
      break;
    case 'D':
      {
        char *class_name =
          sp_intern_string(sp, _get_string_before_symbol(&cursor, '#', &stop, 0));

        char *method_name =
          sp_intern_string(sp, "~");

        ic->ity = VirtualMethodCall;
        ic->class_name = class_name;
        ic->method_name = method_name;
      }
      break;
    case 'P':
      {
        char *class_name =
          sp_intern_string(sp, _get_string_before_symbol(&cursor, '#', &stop, 0));

        size_t len;

        char *co = _get_string_before_symbol(&cursor, '#', &stop, &len);

        if (len == 2)
          ic->attrs = CONSTANT | VOLATILE;
        else if (len == 1) {
          if (*co == 'c') ic->attrs = CONSTANT;
          else            ic->attrs = VOLATILE;
        }

        ic->type =
          sp_intern_string(sp, _get_string_before_symbol(&cursor, '#', &stop, 0));

        ic->ity = PointerToMethodCall;
        ic->class_name = class_name;
      }
      break;
    case 'N':
      {
        ic->type =
          sp_intern_string(sp, _get_string_before_symbol(&cursor, '#', &stop, 0));

        ic->ity = NormalCall;
      }
      break;
    default:
      dprintf(STDERR_FILENO, "ICF parsing failed at %lu\n", cursor - content);
      quit(-1);
      break;
    }
  }
}

/**
 * parse functions whose addresses are taken.
 */
static void parse_fats(char *content, const char *end, /*out*/dict **fats,
                       /*out*/str **sp) {

  char *cursor = content;

  int stop;

  while (cursor < end) {
    char *fat =
      sp_intern_string(sp, _get_string_before_symbol(&cursor, '\0', &stop, 0));

    keyvalue *kv = dict_find(*fats, fat);

    if (!kv)
      dict_add(fats, fat, 0);
  }
}

/**
 * Function aliases.
 */
static void parse_aliases(char *content, const char *end, /*out*/graph **aliases,
                          /*out*/str **sp) {
  char *cursor = content;

  int stop;

  while (cursor < end) {
    char *name = sp_intern_string(sp, _get_string_before_symbol(&cursor, ' ', &stop, 0));

    char *aliasee = sp_intern_string(sp, _get_string_before_symbol(&cursor, ' ', &stop, 0));

    g_add_edge(aliases, name, aliasee);
  }
}

/**
 * split a char stream delimited by "local_symbol" and add the splitted
 * content to node_list.
 */
static void _add_node_list(char *cursor, char local_symbol, /*out*/node **node_list,
                           /*out*/str **sp) {
  int stop;

  do {
    char *name =
      sp_intern_string(sp, _get_string_before_symbol(&cursor, local_symbol, &stop, 0));

    DL_APPEND(*node_list, new_node(name));

  } while (!stop);
}

static void parse_functions(char *content, const char *end, function **functions,
                            /*out*/str **sp) {
  char *cursor = content;

  char *local_cursor = 0;

  function *f = 0;

  int stop;

  while (cursor < end) {
    stop = FALSE;

    while (!stop) {
      switch (*cursor) {
      case '{':
        cursor += 2;
        f = alloc_function();
        f->name = sp_intern_string(sp, _get_string_before_symbol(&cursor, '\n', &stop, 0));
        /* dprintf(STDERR_FILENO, "%s, %lx\n", f->name, f->name); */
        break;
      case 'N':
        cursor += 2;
        f->class_name =
          sp_intern_string(sp, _get_string_before_symbol(&cursor, '#', &stop, 0));
        f->method_name =
          sp_intern_string(sp, _get_string_before_symbol(&cursor, '\n', &stop, 0));
        break;
      case 'D':
        cursor += 2;
        f->class_name =
          sp_intern_string(sp, _get_string_before_symbol(&cursor, '\n', &stop, 0));
        f->method_name = sp_intern_string(sp, "~");
        break;
      case 'Y':
        cursor += 2;
        f->type =
          sp_intern_string(sp, _get_string_before_symbol(&cursor, '\n', &stop, 0));
        break;
      case 'T':
        cursor += 2;
        local_cursor = _get_string_before_symbol(&cursor, '\n', &stop, 0);
        _add_node_list(local_cursor, ' ', &(f->dtails), sp);
        break;
      case 'I':
        cursor += 2;
        local_cursor = _get_string_before_symbol(&cursor, '\n', &stop, 0);
        _add_node_list(local_cursor, ' ', &(f->itails), sp);
        break;
      case 'R':
        cursor += 2;
        local_cursor = _get_string_before_symbol(&cursor, '\n', &stop, 0);
        _add_node_list(local_cursor, ' ', &(f->returns), sp);
        break;
      case '}':
        cursor += 2;
        DL_APPEND(*functions, f);
        f = 0;
        stop = TRUE;
        break;
      default:
        dprintf(STDERR_FILENO, "Functions parsing failed at %lu\n", cursor - content);
        quit(-1);
      }
    }
  }
}

/**
 * Test whether a function is a C++ instance method (non-static member method),
 * and set the attributes if it is.
 */
static int _is_instance_method(const function *f, dict *classes,
                              /*out*/unsigned char *attrs) {
  if (classes && f->class_name && f->method_name) {
    keyvalue *class_entry = dict_find(classes, f->class_name);

    if (!class_entry)
      return FALSE;

    keyvalue *method_entry = dict_find((dict*)(class_entry->value), f->method_name);

    if (!method_entry)
      return FALSE;

    if (attrs)
      *attrs = (unsigned char)method_entry->value;

    return !is_static((unsigned char)method_entry->value);
  }
  return FALSE;
}

/* mark a pointer to an indirect branch */
static void* _mark_ptr(void *ptr) {
  unsigned long p = (unsigned long)ptr;
  p |= 0x8000000000000000UL;
  return (void*)p;
}

/* unmark a pointer */
static void* _unmark_ptr(void *ptr) {
  unsigned long p = (unsigned long)ptr;
  p &= 0x7FFFFFFFFFFFFFFFUL;
  return (void*)p;
};

/* check if a pointer is marked */
static int _is_marked(const void *ptr) {
  return !!((unsigned long)ptr & 0x8000000000000000UL);
}

/**
 * Based on the class hierarchy, find the relations of all virtual
 * methods and output the relations into the call graph.
 */
static void _build_cha_relations(/*out*/graph **callgraph,
                                 dict *classes, graph *cha,
                                 dict *all_virtual_funcs_grouped_by_cls_mtd_name) {
  node *cha_lcc = g_get_lcc(&cha); /* class groups associated by inheritance */
  graph *explored = 0;
  node *n, *ntmp;
  graph *v, *tmp;

  /* for each class inheritance group */
  DL_FOREACH_SAFE(cha_lcc, n, ntmp) {
    /* for each class in the inheritance group */
    HASH_ITER(hh, (vertex*)(n->val), v, tmp) {
      /* for each method of this class, link it to other methods
       * of other classes */
      keyvalue *class_entry = dict_find(classes, v->key);

      if (class_entry) {
        keyvalue *method_entry, *mtmp;

        HASH_ITER(hh, (vertex*)(class_entry->value), method_entry, mtmp) {

          /* only virtual methods are considered */
          if (is_virtual((unsigned char)(method_entry->value)) &&
              !g_directed_edge_in(explored,
                                  class_entry->key,
                                  method_entry->key)) {
            dict *eqc = 0;
            graph *vc, *vctmp;

            HASH_ITER(hh, (vertex*)(n->val), vc, vctmp) {

              keyvalue *i_class_entry = dict_find(classes, vc->key);

              if (i_class_entry) {
                keyvalue *i_method_entry =
                  dict_find((vertex*)(i_class_entry->value), method_entry->key);

                if (i_method_entry && /* this class contains this method */
                    /* this method is virtual */
                    is_virtual((unsigned char)i_method_entry->value)&&
                    /* not explored yet */
                    !g_directed_edge_in(explored, i_class_entry->key, i_method_entry->key)) {
                  g_add_directed_edge(&explored, i_class_entry->key, i_method_entry->key);

                  keyvalue *virtual_method_entry =
                    g_find_l2_vertex(all_virtual_funcs_grouped_by_cls_mtd_name,
                                     i_class_entry->key, i_method_entry->key);

                  if (virtual_method_entry) {
                    keyvalue *method_name, *tmpkv;

                    HASH_ITER(hh, (dict*)(virtual_method_entry->value),
                              method_name, tmpkv) {
                      dict_add(&eqc, method_name->key, 0);
                    }
                  }
                }
              }
            }
            /* add the eqc into the callgraph, eqc will also be freed */
            g_add_cc_to_g(callgraph, eqc);
          }
        }
      }
    }
    /* clean the node in cha_lcc*/
    DL_DELETE(cha_lcc, n);
    dict_clear((dict**)(&(n->val)));
    free(n);
  }
}

static void merge_icfs(/*out*/icf **icfs, icf *mic) {
  icf *ic, *tmp;
  HASH_ITER(hh, mic, ic, tmp) {
    /* copy the info of ic */
    icf *newic = alloc_icf();
    memcpy(newic, ic, sizeof(*ic));
    /* insert the new ic into icfs */
    HASH_ADD_PTR(*icfs, id, newic);
  }
}

static void merge_functions(/*out*/function **functions, function *mf) {
  function *f;
  DL_FOREACH(mf, f) {
    /* copy the info of func */
    function *newf = alloc_function();
    memcpy(newf, f, sizeof(*f));
    //dprintf(STDERR_FILENO, "%s\n", f->name);
    DL_APPEND(*functions, newf);
  }
}

static void print_classes(dict *classes) {
  keyvalue *class_entry, *ctmp;
  HASH_ITER(hh, classes, class_entry, ctmp) {
    dprintf(STDERR_FILENO, "%s\n", class_entry->key);
    dict *method_entry = (dict*)(class_entry->value);
    keyvalue *m, *mtmp;
    HASH_ITER(hh, method_entry, m, mtmp) {
      dprintf(STDERR_FILENO, "  %s\n", m->key);
    }
  }
}

static void merge_dicts(/*out*/dict **d, dict *mc) {
  keyvalue *c, *tmp;
  HASH_ITER(hh, mc, c, tmp) {
    /* copy the info of classes */
    keyvalue *kv = dict_find(*d, c->key);
    /*TODO: here we assume that if two entries have the same key,
     *      their values are equivalent */
    if (!kv) {
      //dprintf(STDERR_FILENO, "%s\n", c->key);
      dict_add(d, c->key, c->value);
    }
  }
}

static void merge_graphs(/*out*/graph **graphs, graph *mg) {
  vertex *v, *tmp;
  HASH_ITER(hh, mg, v, tmp) {
    g_add_vertex(graphs, v->key);
    vertex *nv = dict_find(*graphs, v->key);
    //dprintf(STDERR_FILENO, "%s\n", v->key);
    vertex *vv, *vtmp;
    HASH_ITER(hh, ((graph*)v->value), vv, vtmp) {
      g_add_vertex(&(nv->value), vv->key);
      //dprintf(STDERR_FILENO, "%s, %s\n", v->key, vv->key);
    }
  }
}

/**
 * Traverse a list of modules and combine all mcfi metainfo.
 */
static void merge_mcfi_metainfo(code_module *modules,
                                /*out*/icf **icfs,
                                /*out*/function **functions,
                                /*out*/dict **classes,
                                /*out*/graph **cha,
                                /*out*/dict **fats,
                                /*out*/graph **aliases) {
  code_module *m;
  DL_FOREACH(modules, m) {
    merge_icfs(icfs, m->icfs);
    merge_functions(functions, m->functions);
    merge_dicts(classes, m->classes);
    merge_graphs(cha, m->cha);
    merge_dicts(fats, m->fats);
    merge_graphs(aliases, m->aliases);
  }
}

/**
 * Based on the CFG-related meta information, build a call graph.
 */
static graph *build_callgraph(icf *icfs, function *functions,
                              dict *classes, graph *cha,
                              dict *fats, graph *aliases,
                              dict **all_funcs_grouped_by_name) {
  graph *callgraph = 0; /* call graph */

  /* the following should be freed at the end of this function */
  dict *all_virtual_funcs_grouped_by_cls_mtd_name = 0;
  dict *global_funcs_grouped_by_types = 0;   /* global or static member functions */
  dict *instance_funcs_grouped_by_types = 0; /* non-static and non-virtual methods */
  dict *instance_funcs_grouped_by_const_types = 0;
  dict *instance_funcs_grouped_by_volatile_types = 0;
  dict *instance_funcs_grouped_by_const_volatile_types = 0;
  /*************************************************************/
  function *f;
  int virtual;
  int instance;
  unsigned char attrs;

  /* Compute aliases transitive closure, need to be freed */
  graph *aliases_tc = g_transitive_closure(&aliases);

  DL_FOREACH(functions, f) {
    {
      /* handle aliased function names */
      g_add_directed_edge(all_funcs_grouped_by_name, f->name, f);

      keyvalue *alias_entry = dict_find(aliases_tc, f->name);
      if (alias_entry) {
        keyvalue *v, *tmp;

        HASH_ITER(hh, (vertex*)(alias_entry->value), v, tmp) {
          g_add_directed_edge(all_funcs_grouped_by_name, v->key, f);
        }
      }
    }

    instance = _is_instance_method(f, classes, &attrs);

    if (is_virtual(attrs)) {
      g_add_directed_l2_edge(&all_virtual_funcs_grouped_by_cls_mtd_name,
                             f->class_name, f->method_name, f->name);
      //dprintf(STDERR_FILENO, "1: %s\n", f->name);
      keyvalue *alias_entry = dict_find(aliases_tc, f->name);
      if (alias_entry) {
        keyvalue *v, *tmp;

        HASH_ITER(hh, (vertex*)(alias_entry->value), v, tmp) {
          g_add_directed_l2_edge(&all_virtual_funcs_grouped_by_cls_mtd_name,
                                 f->class_name, f->method_name, v->key);
          //dprintf(STDERR_FILENO, "2: %s\n", v->key);
        }
      }
    }

    if (dict_in(fats, f->name) && f->type) {
      if (!instance) {/* global or static member functions */
        //dprintf(STDERR_FILENO, "%s, %s\n", f->name, f->type);
        g_add_directed_edge(&global_funcs_grouped_by_types,
                            f->type, f->name);
      } else {
        if (!is_constant(attrs) && !is_volatile(attrs)) {
          g_add_directed_edge(&instance_funcs_grouped_by_types,
                              f->type, f->name);
          /*
           * Some destructors are aliased to others, and we should take care
           * of all alises
           */
          keyvalue *alias_entry = dict_find(aliases_tc, f->name);
          if (alias_entry) {
            keyvalue *v, *tmp;

            HASH_ITER(hh, (vertex*)(alias_entry->value), v, tmp) {
              g_add_directed_edge(&instance_funcs_grouped_by_types,
                                  f->type, v->key);
            }
          }
        } else if (is_constant(attrs) && !is_volatile(attrs))
          g_add_directed_edge(&instance_funcs_grouped_by_const_types,
                              f->type, f->name);
        else if (!is_constant(attrs) && is_volatile(attrs))
          g_add_directed_edge(&instance_funcs_grouped_by_volatile_types,
                              f->type, f->name);
        else
          g_add_directed_edge(&instance_funcs_grouped_by_const_volatile_types,
                              f->type, f->name);
      }
    }
  }

  graph *chacc = g_transitive_closure(&cha);
  //g_print_trantive_closure(chacc);
  /* get the list of inheritance relationship */
  _build_cha_relations(&callgraph, classes, cha,
                       all_virtual_funcs_grouped_by_cls_mtd_name);
  
  /* for each indirect branch, find its targets */
  icf *ic, *tmpic;

  HASH_ITER(hh, icfs, ic, tmpic) {
    if (ic->ity == VirtualMethodCall) {

      keyvalue *virtual_method_entry =
        g_find_l2_vertex(all_virtual_funcs_grouped_by_cls_mtd_name,
                         ic->class_name, ic->method_name);

      if (virtual_method_entry) {
        keyvalue *method_name, *tmpkv;

        HASH_ITER(hh, (dict*)(virtual_method_entry->value),
                  method_name, tmpkv) {
          g_add_edge(&callgraph, _mark_ptr(ic->id), method_name->key);
          //dprintf(STDERR_FILENO, "%s, %s\n", ic->id, method_name->key);
        }
      } else {
        /* it is possible that if the class_name::method_name is inlined
           at all call sites so that no class_name::method_name function
           would appear in the final binary. we should check all equivalent
           classes of this class to see if their same-name virtual methods
           are there */
        keyvalue *class_inheritance_group = dict_find(chacc, ic->class_name);
        if (class_inheritance_group) {
          keyvalue *k, *tmp;
          HASH_ITER(hh, (dict*)(class_inheritance_group->value), k, tmp) {
            keyvalue *virtual_method_entry =
              g_find_l2_vertex(all_virtual_funcs_grouped_by_cls_mtd_name,
                               k->key, ic->method_name);
            if (virtual_method_entry) {
              keyvalue *method_name, *tmpkv;
              
              HASH_ITER(hh, (dict*)(virtual_method_entry->value),
                        method_name, tmpkv) {
                g_add_edge(&callgraph, _mark_ptr(ic->id), method_name->key);
              }
              break;
            }
          }
        }
      }
    } else if (ic->ity == PointerToMethodCall) {
      dict *g = 0;

      /*
       * TODO: it is also possible that some virtual functions possibly pointed
       *       to by a method pointer are completely inlined and no entry is
       *       emitted. This is rare and I will fix this problem later.
       */
      if (!is_constant(ic->attrs) && !is_volatile(ic->attrs))
        g = instance_funcs_grouped_by_types;
      else if (is_constant(ic->attrs) && !is_volatile(ic->attrs))
        g = instance_funcs_grouped_by_const_types;
      else if (!is_constant(ic->attrs) && is_volatile(ic->attrs))
        g = instance_funcs_grouped_by_volatile_types;
      else
        g = instance_funcs_grouped_by_const_volatile_types;

      keyvalue *typed_methods = dict_find(g, ic->type);

      if (typed_methods) {
        keyvalue *v, *tmp;
        HASH_ITER(hh, (dict*)(typed_methods->value), v, tmp) {
          g_add_edge(&callgraph, _mark_ptr(ic->id), v->key);
        }
      }
    } else {
      keyvalue *funcs_with_same_type = dict_find(global_funcs_grouped_by_types, ic->type);
      keyvalue *v, *tmp;

      if (funcs_with_same_type) {
        HASH_ITER(hh, (dict*)(funcs_with_same_type->value), v, tmp) {
          //dprintf(STDERR_FILENO, "%s, %s\n", ic->id, v->key);
          g_add_edge(&callgraph, _mark_ptr(ic->id), v->key);
        }
      }
    }
  }
  /* free the following graphs */
  g_dtor(&all_virtual_funcs_grouped_by_cls_mtd_name);
  g_dtor(&global_funcs_grouped_by_types);
  g_dtor(&instance_funcs_grouped_by_types);
  g_dtor(&instance_funcs_grouped_by_const_types);
  g_dtor(&instance_funcs_grouped_by_volatile_types);
  g_dtor(&instance_funcs_grouped_by_const_volatile_types);
  /* free the transitive closure of aliases */
  g_free_transitive_closure(&aliases_tc);
  g_free_transitive_closure(&chacc);
  return callgraph;
}

static unsigned long _convert_to_mcfi_half_id_format(unsigned long *number) {
  unsigned long a, b, c, d;

  while (TRUE) {
    *number %= 268435455UL; /* 2^28-1 */
    a = (*number & 127);
    a <<= 1;
    b = (*number >> 7) & 127;
    b <<= 1;
    c = (*number >> 14) & 127;
    c <<= 1;
    d = (*number >> 21) & 127;
    d <<= 1;
    ++*number;
    if (a != 0xf4 && b != 0xf4 && c != 0xf4 && d != 0xf4)
      break;
  }
  unsigned long rs = ((d << 24) | (c << 16) | (b << 8) | a);
  assert(rs != 0xFFFFFFFFUL);
  return rs;
}

static graph *build_retgraph(graph *callgraph, dict *all_funcs_grouped_by_name) {
  graph *retgraph = 0;
  return retgraph;
}

static dict *gen_mcfi_id(node *lcc, /*out*/unsigned long *version) {
  node *n, *ntmp;

  dict *rs = 0;

  unsigned long eqc_number = 0;

  unsigned long mcfi_version = _convert_to_mcfi_half_id_format(version);

  DL_FOREACH_SAFE(lcc, n, ntmp) {
    vertex *v, *tmp;

    unsigned long id = _convert_to_mcfi_half_id_format(&eqc_number);

    id = ((id << 32UL) | mcfi_version | 1); /* least significant bit should be one */
    
    HASH_ITER(hh, (vertex*)(n->val), v, tmp) {
      dict_add(&rs, v->key, (void*)id);
    }
  }

  return rs;
}

/* generate and populate the tary table for module m */
static void gen_tary(code_module *m, dict *callids, char *table) {
  char *tary = table + m->base_addr;
  symbol *funcsym;
  DL_FOREACH(m->funcsyms, funcsym) {
    keyvalue *f = dict_find(callids, funcsym->name);
    //dprintf(STDERR_FILENO, "%s, %x\n", funcsym->name, funcsym->offset);
    if (f) {
      //dprintf(STDERR_FILENO, "%s, %x, %lx\n", funcsym->name, funcsym->offset, f->value);
      *((unsigned long*)(tary + funcsym->offset)) = (unsigned long)f->value;
    }
  }
}

/* generate and populate the bary table for module m */
static void gen_bary(code_module *m, dict *callids, char *table) {
  symbol *icfsym;
  DL_FOREACH(m->icfsyms, icfsym) {
    keyvalue *i = dict_find(callids, _mark_ptr(icfsym->name));
    if (i) {
      //dprintf(STDERR_FILENO, "%s, %x, %lx\n", icfsym->name, icfsym->offset, i->value);
      *((unsigned long*)(table + icfsym->offset)) = (unsigned long)i->value;
    }
  }
}

#endif