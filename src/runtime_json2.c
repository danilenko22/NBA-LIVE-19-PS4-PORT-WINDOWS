/* SacredFire: libSceJson2 entry points the game imports. The game parses JSON with its own code
 * and only touches the library's set-up classes: it derives its allocator from
 * sce::Json::MemAllocator (an abstract base: virtual allocate/deallocate/notifyError) and runs
 * sce::Json::Initializer::initialize/terminate with it. The base constructor installs a vtable
 * whose entries must never run (the derived class overrides them); notifyError is the base
 * class's default, which only reports. Itanium C++ ABI: `this` is the first argument. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

static ABI void pure_virtual(void) {
    fputs("STOP: sce::Json::MemAllocator base virtual called (the game's override is missing)\n",stderr);
    exit(21);
}
static ABI void base_dtor(void *self) { (void)self; }
/* Default notifyError: the library reports allocation failures through the allocator; nothing to do. */
static _Atomic unsigned notified;
static ABI void notify_error(void *self,int32_t code,uint64_t size,void *user) {
    (void)self; (void)user;
    if (atomic_fetch_add_explicit(&notified,1,memory_order_relaxed)<4) printf("Json2: MemAllocator::notifyError(%d, %llu bytes)\n",code,(unsigned long long)size);
}
/* Vtable: offset-to-top, typeinfo, then the virtual functions in declaration order:
 * ~MemAllocator (complete and deleting), allocate, deallocate, notifyError. */
static void *const memallocator_vtable[]={
    0,0,(void *)base_dtor,(void *)base_dtor,(void *)pure_virtual,(void *)pure_virtual,(void *)notify_error,
};
static ABI void memallocator_ctor(void **self) { if (self) *self=(void *)&memallocator_vtable[2]; }
static ABI void memallocator_dtor(void *self) { (void)self; }

/* Initializer: records the allocator the game hands in; the object itself holds nothing we use. */
typedef struct { void *allocator, *user; uint64_t buffer_size; } JsonInitParameter;
static const JsonInitParameter *json_parameter;
static ABI void initializer_ctor(void *self) { (void)self; }
static ABI void initializer_dtor(void *self) { (void)self; }
static ABI int32_t initializer_initialize(void *self,const JsonInitParameter *p) {
    (void)self;
    if (!p || !p->allocator) return (int32_t)0x80920000|1; /* invalid argument */
    json_parameter=p;
    printf("Json2: initialized with the game's allocator (buffer %llu bytes)\n",(unsigned long long)p->buffer_size);
    return 0;
}
static ABI int32_t initializer_terminate(void *self) { (void)self; json_parameter=NULL; return 0; }

/* sce::Json::Value / String / Parser: NBA LIVE 19 parses PSN store replies with them. The guest
 * objects only hold one pointer to our node (the first 8 bytes, whatever their real size), so
 * nothing is written past it. ValueType: null, boolean, integer, uinteger, real, string, array,
 * object. A missing key or index gives a shared null value. Nodes belong to the root Value the
 * game constructed and are freed by its clear/destructor. */
enum { JNULL, JBOOL, JINT, JUINT, JREAL, JSTRING, JARRAY, JOBJECT };
typedef struct JNode JNode;
typedef struct { JNode *node; } JHolder;       /* what a guest Value/String holds */
typedef struct { char *text; } JString;
struct JNode {
    int type; int64_t i; double r; int b;
    JString str;                                /* JSTRING value, as a guest String */
    char **keys; JNode **items; JHolder *holders; size_t count;
};
static JNode json_null_node={JNULL,0,0,0,{(char *)""},NULL,NULL,NULL,0};
static JHolder json_null_value={&json_null_node};

static void jnode_free(JNode *n) {
    if (!n || n==&json_null_node) return;
    for (size_t k=0;k<n->count;++k) { jnode_free(n->items[k]); if (n->keys) free(n->keys[k]); }
    free(n->keys); free(n->items); free(n->holders);
    if (n->type==JSTRING) free(n->str.text);
    free(n);
}
static JNode *jnode_new(int type) { JNode *n=calloc(1,sizeof *n); if (n) n->type=type; return n; }
static int jnode_add(JNode *n,char *key,JNode *child) {
    char **keys=n->type==JOBJECT ? realloc(n->keys,(n->count+1)*sizeof *keys) : NULL;
    if (n->type==JOBJECT) { if (!keys) return -1; n->keys=keys; keys[n->count]=key; }
    JNode **items=realloc(n->items,(n->count+1)*sizeof *items); if (!items) return -1; n->items=items;
    items[n->count++]=child;
    return 0;
}
static void jnode_finish(JNode *n) { /* holders for operator[] once the children are final */
    if (!n->count) return;
    n->holders=calloc(n->count,sizeof *n->holders);
    for (size_t k=0;n->holders && k<n->count;++k) n->holders[k].node=n->items[k];
}

typedef struct { const char *p, *end; int depth; } JParse;
static void jskip(JParse *s) { while (s->p<s->end && (*s->p==' '||*s->p=='\t'||*s->p=='\n'||*s->p=='\r')) ++s->p; }
static char *jparse_string(JParse *s) {
    if (s->p>=s->end || *s->p!='"') return NULL;
    ++s->p;
    size_t cap=16, n=0; char *out=malloc(cap); if (!out) return NULL;
    while (s->p<s->end && *s->p!='"') {
        unsigned c=(unsigned char)*s->p++;
        if (c=='\\' && s->p<s->end) {
            c=(unsigned char)*s->p++;
            switch (c) {
            case 'n': c='\n'; break; case 't': c='\t'; break; case 'r': c='\r'; break;
            case 'b': c='\b'; break; case 'f': c='\f'; break;
            case 'u': {
                unsigned v=0; for (int k=0;k<4 && s->p<s->end;++k) { char h=*s->p++; v=v*16+(unsigned)(h<='9'?h-'0':(h|32)-'a'+10); }
                char u[4]; int m=0;
                if (v<0x80) u[m++]=(char)v; else if (v<0x800) { u[m++]=(char)(0xc0|v>>6); u[m++]=(char)(0x80|(v&63)); }
                else { u[m++]=(char)(0xe0|v>>12); u[m++]=(char)(0x80|((v>>6)&63)); u[m++]=(char)(0x80|(v&63)); }
                for (int k=0;k<m;++k) { if (n+2>cap) { char *g=realloc(out,cap*=2); if (!g) { free(out); return NULL; } out=g; } out[n++]=u[k]; }
                continue;
            }
            default: break; /* \" \\ \/ */
            }
        }
        if (n+2>cap) { char *g=realloc(out,cap*=2); if (!g) { free(out); return NULL; } out=g; }
        out[n++]=(char)c;
    }
    if (s->p>=s->end) { free(out); return NULL; }
    ++s->p; out[n]=0;
    return out;
}
static JNode *jparse_value(JParse *s) {
    jskip(s);
    if (s->p>=s->end || ++s->depth>64) return NULL;
    JNode *n=NULL;
    char c=*s->p;
    if (c=='{' || c=='[') {
        const int object=c=='{'; const char close=object?'}':']';
        n=jnode_new(object?JOBJECT:JARRAY); if (!n) return NULL;
        ++s->p; jskip(s);
        if (s->p<s->end && *s->p==close) ++s->p;
        else for (;;) {
            char *key=NULL;
            if (object) {
                jskip(s); key=jparse_string(s); jskip(s);
                if (!key || s->p>=s->end || *s->p!=':') { free(key); jnode_free(n); return NULL; }
                ++s->p;
            }
            JNode *child=jparse_value(s);
            if (!child || jnode_add(n,key,child)) { free(key); jnode_free(child); jnode_free(n); return NULL; }
            jskip(s);
            if (s->p<s->end && *s->p==',') { ++s->p; continue; }
            if (s->p<s->end && *s->p==close) { ++s->p; break; }
            jnode_free(n); return NULL;
        }
        jnode_finish(n);
    } else if (c=='"') {
        char *t=jparse_string(s); if (!t) return NULL;
        n=jnode_new(JSTRING); if (!n) { free(t); return NULL; } n->str.text=t;
    } else if (s->end-s->p>=4 && !strncmp(s->p,"true",4)) { n=jnode_new(JBOOL); if (n) n->b=1; s->p+=4; }
    else if (s->end-s->p>=5 && !strncmp(s->p,"false",5)) { n=jnode_new(JBOOL); s->p+=5; }
    else if (s->end-s->p>=4 && !strncmp(s->p,"null",4)) { n=jnode_new(JNULL); s->p+=4; }
    else {
        char num[64]; int k=0, real=0;
        while (s->p<s->end && k<63 && strchr("+-0123456789.eE",*s->p)) { if (strchr(".eE",*s->p)) real=1; num[k++]=*s->p++; }
        num[k]=0;
        if (!k) return NULL;
        n=jnode_new(real?JREAL:JINT); if (!n) return NULL;
        if (real) n->r=strtod(num,NULL); else n->i=strtoll(num,NULL,10);
        if (!real && num[0]!='-' && strtoull(num,NULL,10)>(unsigned long long)INT64_MAX) { n->type=JUINT; n->i=(int64_t)strtoull(num,NULL,10); }
    }
    --s->depth;
    return n;
}

static JNode *jnode_of(const void *self) { const JNode *n=self ? ((const JHolder *)self)->node : NULL; return n ? (JNode *)n : &json_null_node; }
static ABI void value_ctor(JHolder *self) { if (self) self->node=NULL; }
static ABI void value_clear(JHolder *self) { if (self) { jnode_free(self->node); self->node=NULL; } }
static ABI void value_dtor(JHolder *self) { value_clear(self); }
static ABI int32_t value_type(const JHolder *self) { return jnode_of(self)->type; }
/* The getters return references (const int64_t& getInteger(), const bool& getBoolean()): the
 * game dereferences the returned pointer (guest +0x21094df), so they point into the node, or at
 * a shared zero for another type. */
static const int64_t json_zero_integer=0;
static const _Bool json_false=0, json_true=1;
static ABI const int64_t *value_integer(const JHolder *self) {
    const JNode *n=jnode_of(self);
    return n->type==JINT || n->type==JUINT ? &n->i : &json_zero_integer;
}
static ABI const _Bool *value_boolean(const JHolder *self) {
    const JNode *n=jnode_of(self);
    return n->type==JBOOL && n->b ? &json_true : &json_false;
}
static ABI const JString *value_string(const JHolder *self) { return &jnode_of(self)->str; }
static ABI uint64_t value_count(const JHolder *self) { const JNode *n=jnode_of(self); return n->type==JARRAY||n->type==JOBJECT ? n->count : 0; }
static ABI const JHolder *value_key(const JHolder *self,const char *key) {
    const JNode *n=jnode_of(self);
    if (n->type==JOBJECT && key && n->holders) for (size_t k=0;k<n->count;++k) if (!strcmp(n->keys[k],key)) return &n->holders[k];
    return &json_null_value;
}
static ABI const JHolder *value_index(const JHolder *self,uint64_t index) {
    const JNode *n=jnode_of(self);
    if ((n->type==JARRAY||n->type==JOBJECT) && n->holders && index<n->count) return &n->holders[index];
    return &json_null_value;
}
static ABI void value_null_callback(void *callback,void *context) { (void)callback; (void)context; } /* null reads use json_null_value */

/* String: one malloc'd C string. */
static ABI void string_ctor(JString *self) { if (self) self->text=NULL; }
static ABI void string_dtor(JString *self) { if (self) { free(self->text); self->text=NULL; } }
static ABI const char *string_cstr(const JString *self) { return self && self->text ? self->text : ""; }

typedef struct { char *p; size_t n, cap; } JOut;
static void jout(JOut *o,const char *s,size_t len) {
    if (o->n+len+1>o->cap) { size_t cap=o->cap?o->cap:64; while (o->n+len+1>cap) cap*=2; char *g=realloc(o->p,cap); if (!g) return; o->p=g; o->cap=cap; }
    memcpy(o->p+o->n,s,len); o->n+=len; o->p[o->n]=0;
}
static void jout_string(JOut *o,const char *s) {
    jout(o,"\"",1);
    for (;*s;++s) {
        char e[8]; unsigned char c=(unsigned char)*s;
        if (c=='"'||c=='\\') { e[0]='\\'; e[1]=(char)c; jout(o,e,2); }
        else if (c<0x20) { snprintf(e,sizeof e,"\\u%04x",c); jout(o,e,6); }
        else jout(o,s,1);
    }
    jout(o,"\"",1);
}
static void jserialize(JOut *o,const JNode *n) {
    char num[48];
    switch (n->type) {
    case JBOOL: jout(o,n->b?"true":"false",n->b?4:5); break;
    case JINT: jout(o,num,(size_t)snprintf(num,sizeof num,"%lld",(long long)n->i)); break;
    case JUINT: jout(o,num,(size_t)snprintf(num,sizeof num,"%llu",(unsigned long long)n->i)); break;
    case JREAL: jout(o,num,(size_t)snprintf(num,sizeof num,"%.17g",n->r)); break;
    case JSTRING: jout_string(o,n->str.text?n->str.text:""); break;
    case JARRAY: case JOBJECT:
        jout(o,n->type==JOBJECT?"{":"[",1);
        for (size_t k=0;k<n->count;++k) {
            if (k) jout(o,",",1);
            if (n->type==JOBJECT) { jout_string(o,n->keys[k]); jout(o,":",1); }
            jserialize(o,n->items[k]);
        }
        jout(o,n->type==JOBJECT?"}":"]",1);
        break;
    default: jout(o,"null",4);
    }
}
static ABI int32_t value_serialize(const JHolder *self,JString *out) {
    if (!out) return (int32_t)0x80920000|1;
    JOut o={0}; jserialize(&o,jnode_of(self));
    free(out->text); out->text=o.p ? o.p : calloc(1,1);
    return 0;
}
/* Parser::parse(Value&, const char *buffer, size_t size): 0, or a parse error. */
static _Atomic unsigned parse_logs;
static ABI int32_t parser_parse(JHolder *value,const char *buffer,uint64_t size) {
    if (!value || !buffer) return (int32_t)0x80920000|1;
    JParse s={buffer,buffer+size,0};
    JNode *n=jparse_value(&s);
    if (atomic_fetch_add_explicit(&parse_logs,1,memory_order_relaxed)<16)
        printf("Json2: parse %llu bytes -> %s\n",(unsigned long long)size,n?"ok":"error");
    if (!n) return (int32_t)0x80920000|0x10; /* parse error */
    jnode_free(value->node); value->node=n;
    return 0;
}

static const RuntimeExport exports[]={
    {"_ZN3sce4Json12MemAllocatorC2Ev",memallocator_ctor}, {"_ZN3sce4Json12MemAllocatorD2Ev",memallocator_dtor},
    {"_ZN3sce4Json12MemAllocator11notifyErrorEimPv",notify_error},
    {"_ZN3sce4Json11InitializerC1Ev",initializer_ctor}, {"_ZN3sce4Json11InitializerD1Ev",initializer_dtor},
    {"_ZN3sce4Json11Initializer10initializeEPKNS0_13InitParameterE",initializer_initialize},
    {"_ZN3sce4Json11Initializer9terminateEv",initializer_terminate},
    {"_ZN3sce4Json5ValueC1Ev",value_ctor}, {"_ZN3sce4Json5ValueD1Ev",value_dtor},
    {"_ZN3sce4Json5Value5clearEv",value_clear},
    {"_ZN3sce4Json5Value9serializeERNS0_6StringE",value_serialize},
    {"_ZN3sce4Json5Value21setNullAccessCallBackEPFRKS1_NS0_9ValueTypeEPS2_PvES6_",value_null_callback},
    {"_ZNK3sce4Json5Value7getTypeEv",value_type}, {"_ZNK3sce4Json5Value10getIntegerEv",value_integer},
    {"_ZNK3sce4Json5Value10getBooleanEv",value_boolean}, {"_ZNK3sce4Json5Value9getStringEv",value_string},
    {"_ZNK3sce4Json5Value5countEv",value_count},
    {"_ZNK3sce4Json5ValueixEPKc",value_key}, {"_ZNK3sce4Json5ValueixEm",value_index},
    {"_ZN3sce4Json6StringC1Ev",string_ctor}, {"_ZN3sce4Json6StringD1Ev",string_dtor},
    {"_ZNK3sce4Json6String5c_strEv",string_cstr},
    {"_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm",parser_parse},
};
uintptr_t runtime_json2_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
