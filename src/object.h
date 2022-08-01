#ifndef clox_object_h
#define clox_object_h

#include "chunk.h"
#include "common.h"
#include "value.h"

#define OBJ_TYPE(value) (AS_OBJ(value)->type)

#define IS_CLOSURE(value)  isObjType(value, OBJ_CLOSURE)
#define IS_FUNCTION(value) isObjType(value, OBJ_FUNCTION)
#define IS_NATIVE(value)   ifObjType(value, OBJ_NATIVE)
#define IS_STRING(value)   isObjType(value, OBJ_STRING)

#define AS_CLOSURE(value)  ((ObjClosure*)AS_OBJ(value))
#define AS_FUNCTION(value) ((ObjFunction*)AS_OBJ(value))
#define AS_NATIVE(value)   (((ObjNative*)AS_OBJ(value))->function)
#define AS_STRING(value)   ((ObjString*)AS_OBJ(value))
#define AS_CSTRING(value)  ((ObjString*)AS_OBJ(value))->chars

typedef enum ObjType {
    OBJ_CLOSURE,
    OBJ_FUNCTION,
    OBJ_NATIVE,
    OBJ_STRING,
    OBJ_UPVALUE,
} ObjType;

typedef struct Obj {
    ObjType type;
    bool isMarked;
    Obj*    next;
} Obj;

typedef struct ObjFunction {
    Obj        obj;
    int        arity;
    int        upvalueCount;
    Chunk      chunk;
    ObjString* name;
} ObjFunction;

typedef Value (*NativeFn)(int argCount, Value* args);

typedef struct ObjNative {
    Obj      obj;
    NativeFn function;
} ObjNative;

typedef struct ObjString {
    Obj      obj;
    int      length;
    char*    chars;
    uint32_t hash;
} ObjString;

typedef struct ObjUpvalue {
    Obj    obj;
    Value* location;
    Value closed;
    struct ObjUpvalue* next;
} ObjUpvalue;

typedef struct ObjClosure {
    Obj          obj;
    ObjFunction* function;
    ObjUpvalue** upvalues;  // Pointer to an array of pointers to upvalues.
    int          upvalueCount;
} ObjClosure;

static inline bool isObjType(Value value, ObjType type) {
    return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

ObjClosure*  newClosure(ObjFunction* function);
ObjFunction* newFunction();
ObjNative*   newNative(NativeFn function);
ObjString*   takeString(char* chars, int length);
ObjString*   copyString(const char* chars, int length);
ObjUpvalue*  newUpvalue(Value* slot);
void         printObject(Value value);

#endif