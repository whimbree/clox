#include "compiler.h"

#include <stdio.h>
#include <string.h>

#include "chunk.h"
#include "common.h"
#include "memory.h"
#include "object.h"
#include "scanner.h"
#include "vm.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

typedef struct Parser {
    Token current;
    Token previous;
    bool  hadError;
    bool  panicMode;
} Parser;

typedef enum Precedence {
    PREC_NONE,
    PREC_ASSIGNMENT,  // =
    PREC_OR,          // or
    PREC_AND,         // and
    PREC_EQUALITY,    // == !=
    PREC_COMPARISON,  // < > <= >=
    PREC_TERM,        // + -
    PREC_FACTOR,      // * /
    PREC_UNARY,       // ! -
    PREC_CALL,        // . ()
    PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct ParseRule {
    ParseFn    prefix;
    ParseFn    infix;
    Precedence precedence;
} ParseRule;

typedef struct Local {
    Token name;
    int   depth;
    bool  isCaptured;
} Local;

typedef struct Upvalue {
    uint8_t index;
    bool    isLocal;
} Upvalue;

typedef enum FunctionType {
    TYPE_FUNCTION,
    TYPE_INITIALIZER,
    TYPE_METHOD,
    TYPE_SCRIPT
} FunctionType;

typedef struct Compiler {
    struct Compiler* enclosing;
    ObjFunction*     function;
    FunctionType     type;

    Local   locals[UINT8_COUNT];
    int     localCount;
    Upvalue upvalues[UINT8_COUNT];
    int     scopeDepth;
} Compiler;

typedef struct ClassCompiler {
    struct ClassCompiler* enclosing;
    Token                 name;
    bool hasSuperclass;
} ClassCompiler;

Parser parser;

Compiler* current = NULL;

ClassCompiler* currentClass = NULL;

Chunk* compilingChunk;

static Chunk* currentChunk() { return &current->function->chunk; }

// ---- START FORWARD DECLARATIONS ---- //
// Error handling
static void errorAt(Token* token, const char* message);
static void error(const char* message);
static void errorAtCurrent(const char* message);

// Parser helpers
static void advance();
static void consume(TokenType type, const char* message);
static bool check(TokenType type);
static bool match(TokenType type);

// Emit bytecode helpers
static void emitByte(uint8_t byte);
static void emitBytes(uint8_t byte1, uint8_t byte2);
static void emitReturn();
static void emitConstant(Value value);
static int  emitJump(uint8_t instruction);
static void patchJump(int offset);
static void emitLoop(int loopStart);

// Variable/constant/scope helpers
static uint8_t makeConstant(Value value);
static uint8_t identifierConstant(Token* name);
static Token syntheticToken(const char* name);
static void    declareVariable();
static bool    identifiersEqual(Token* a, Token* b);
static int     resolveLocal(Compiler* compiler, Token* name);
static int     resolveUpvalue(Compiler* compiler, Token* name);
static void    addLocal(Token name);
static int     addUpvalue(Compiler* compiler, uint8_t index, bool isLocal);
static void    namedVariable(Token name, bool canAssign);
static void    defineVariable(uint8_t global);
static uint8_t argumentList();
static void    markInitialized();
static uint8_t parseVariable(const char* errorMessage);
static void    beginScope();
static void    endScope();

// Driving functions
static void         initCompiler(Compiler* compiler, FunctionType type);
ObjFunction*        compile(const char* source);
static ObjFunction* endCompiler();

// Recursive descent
static void expression();
static void statement();
static void declaration();
static void varDeclaration();
static void funDeclaration();
static void expressionStatement();
static void printStatement();
static void returnStatement();
static void synchronize();
static void block();
static void function(FunctionType type);
static void method();
static void classDeclaration();
static void ifStatement();
static void whileStatment();
static void forStatement();

// Pratt parsing
ParseRule         rules[];
static ParseRule* getRule(TokenType operatorType);
static void       parsePrecedence(Precedence precedence);
static void       binary(bool canAssign);
static void       unary(bool canAssign);
static void       call(bool canAssign);
static void       dot(bool canAssign);
static void       grouping(bool canAssign);
static void       number(bool canAssign);
static void       string(bool canAssign);
static void       literal(bool canAssign);
static void       variable(bool canAssign);
static void       this_(bool canAssign);
static void       and_(bool canAssign);
static void       or_(bool canAssign);
static void super_(bool canAssign);
// ---- END FORWARD DECLARATIONS ---- //

static void errorAt(Token* token, const char* message) {
    if (parser.panicMode) return;
    parser.panicMode = true;

    fprintf(stderr, "[line %d] Error", token->line);

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type == TOKEN_ERROR) {
        // Nothing.
    } else {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    parser.hadError = true;
}

static void error(const char* message) { errorAt(&parser.previous, message); }

static void errorAtCurrent(const char* message) { errorAt(&parser.current, message); }

static void advance() {
    parser.previous = parser.current;

    for (;;) {
        parser.current = scanToken();
        if (parser.current.type != TOKEN_ERROR) break;

        errorAtCurrent(parser.current.start);
    }
}

static void consume(TokenType type, const char* message) {
    if (parser.current.type == type) {
        advance();
        return;
    }

    errorAtCurrent(message);
}

static bool check(TokenType type) { return parser.current.type == type; }

static bool match(TokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

static void emitByte(uint8_t byte) { writeChunk(currentChunk(), byte, parser.previous.line); }

static void emitBytes(uint8_t byte1, uint8_t byte2) {
    emitByte(byte1);
    emitByte(byte2);
}

static void emitReturn() {
    // In an initializer, instead of pushing nil onto the stack before returning, we
    // load slot zero, which contains the instance. This emitReturn() function is
    // also called when compiling a return statement without a value, so this also
    // correctly handles cases where the user does an early return inside the initializer.
    if (current->type == TYPE_INITIALIZER) {
        emitBytes(OP_GET_LOCAL, 0);
    } else {
        emitByte(OP_NIL);
    }

    emitByte(OP_RETURN);
}

// Adds a constant to the chunk's dynamic value array, and returns the index to constant
static uint8_t makeConstant(Value value) {
    int constant = addConstant(currentChunk(), value);
    if (constant > UINT8_MAX) {
        error("Too many constants in one chunk.");
        return 0;
    }

    return (uint8_t)constant;
}

static void emitConstant(Value value) { emitBytes(OP_CONSTANT, makeConstant(value)); }

static int emitJump(uint8_t instruction) {
    emitByte(instruction);
    emitBytes(0xff, 0xff);
    return currentChunk()->count - 2;
}

static void patchJump(int offset) {
    // Offset is the index where the jump instruction itself is written.

    // -2 to adjust for the bytecode for the jump offset itself.
    int jump = currentChunk()->count - offset - 2;

    if (jump > UINT16_MAX) {
        error("Too much code to jump over.");
    }

    // Jump offset stored big-endian.
    currentChunk()->code[offset]     = (jump >> 8) & 0xff;
    currentChunk()->code[offset + 1] = jump & 0xff;
}

// Emit a new loop instruction which unconditionally jumps backwards to a given offset.
static void emitLoop(int loopStart) {
    emitByte(OP_LOOP);

    // The +2 is to take into account the size of the OP_LOOP instructions's operands.
    int offset = currentChunk()->count - loopStart + 2;
    if (offset > UINT16_MAX) error("Loop body too large.");

    emitBytes((offset >> 8) & 0xff, offset & 0xff);
}

static void initCompiler(Compiler* compiler, FunctionType type) {
    compiler->enclosing  = current;
    compiler->function   = newFunction();
    compiler->type       = type;
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    current              = compiler;

    if (type != TYPE_SCRIPT) {
        compiler->function->name = copyString(parser.previous.start, parser.previous.length);
    }

    Local* local      = &current->locals[current->localCount++];
    local->depth      = 0;
    local->isCaptured = false;
    if (type != TYPE_FUNCTION) {
        local->name.start  = "this";
        local->name.length = 4;
    } else {
        local->name.start  = "";
        local->name.length = 0;
    }
}

static ObjFunction* endCompiler() {
    emitReturn();
    ObjFunction* function = current->function;

#ifdef DEBUG_PRINT_CODE
    if (!parser.hadError) {
        disassembleChunk(currentChunk(),
                         function->name != NULL ? function->name->chars : "<script>");
    }
#endif

    current = current->enclosing;
    return function;
}

static void beginScope() { current->scopeDepth++; }

static void endScope() {
    current->scopeDepth--;

    while (current->localCount >= 0 &&
           current->locals[current->localCount - 1].depth > current->scopeDepth) {
        if (current->locals[current->localCount - 1].isCaptured) {
            emitByte(OP_CLOSE_UPVALUE);
        } else {
            emitByte(OP_POP);
        }

        current->localCount--;
    }
}

static void binary(bool canAssign) {
    // Remember the operator.
    TokenType operatorType = parser.previous.type;

    // Compile the right operand.
    ParseRule* rule = getRule(operatorType);
    parsePrecedence((Precedence)(rule->precedence + 1));

    // Emit the operator instruction.
    switch (operatorType) {
        case TOKEN_BANG_EQUAL: emitBytes(OP_EQUAL, OP_NOT); break;
        case TOKEN_EQUAL_EQUAL: emitByte(OP_EQUAL); break;
        case TOKEN_GREATER: emitByte(OP_GREATER); break;
        case TOKEN_GREATER_EQUAL: emitBytes(OP_LESS, OP_NOT); break;
        case TOKEN_LESS: emitByte(OP_LESS); break;
        case TOKEN_LESS_EQUAL: emitBytes(OP_GREATER, OP_NOT); break;
        case TOKEN_PLUS: emitByte(OP_ADD); break;
        case TOKEN_MINUS: emitByte(OP_SUBTRACT); break;
        case TOKEN_STAR: emitByte(OP_MULTIPLY); break;
        case TOKEN_SLASH: emitByte(OP_DIVIDE); break;
        default: return;  // Unreachable.
    }
}

static void unary(bool canAssign) {
    TokenType operatorType = parser.previous.type;

    // Compile the operand.
    parsePrecedence(PREC_UNARY);

    // Emit the operator instruction.
    switch (operatorType) {
        case TOKEN_BANG: emitByte(OP_NOT); break;
        case TOKEN_MINUS: emitByte(OP_NEGATE); break;
        default: return;  // Unreachable.
    }
}

static void call(bool canAssign) {
    uint8_t argCount = argumentList();
    emitBytes(OP_CALL, argCount);
}

static void dot(bool canAssign) {
    consume(TOKEN_IDENTIFIER, "Expect property name after '.'.");
    uint8_t name = identifierConstant(&parser.previous);

    // The parser may call dot() in a context that is too high
    // precedence to permit a setter to appear. To avoid incorrectly allowing that, we
    // only parse and compile the equals part when canAssign is true. If an equals
    // token appears when canAssign is false, dot() leaves it alone and returns. In
    // that case, the compiler will eventually unwind up to parsePrecedence()
    // which stops at the unexpected = still sitting as the next token and reports an
    // error.
    if (canAssign && match(TOKEN_EQUAL)) {
        expression();
        emitBytes(OP_SET_PROPERTY, name);
    }else if (match(TOKEN_LEFT_PAREN)) {
        uint8_t argCount = argumentList();
        emitBytes(OP_INVOKE, name);
        emitByte(argCount);
    } else {
        emitBytes(OP_GET_PROPERTY, name);
    }
}

static void grouping(bool canAssign) {
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void number(bool canAssign) {
    double value = strtod(parser.previous.start, NULL);
    emitConstant(NUMBER_VAL(value));
}

static void string(bool canAssign) {
    emitConstant(OBJ_VAL(copyString(parser.previous.start + 1, parser.previous.length - 2)));
}

static void literal(bool canAssign) {
    switch (parser.previous.type) {
        case TOKEN_FALSE: emitByte(OP_FALSE); break;
        case TOKEN_NIL: emitByte(OP_NIL); break;
        case TOKEN_TRUE: emitByte(OP_TRUE); break;
        default: return;  // Unreachable.
    }
}

static void parsePrecedence(Precedence precedence) {
    advance();
    ParseFn prefixRule = getRule(parser.previous.type)->prefix;
    if (prefixRule == NULL) {
        error("Expect expression.");
        return;
    }

    /* If the variable happens to be the right-hand side of an infix operator,
    or the operand of a unary operator,
    then that containing expression is too high precedence to permit the '='.
    To fix this, variable() should only look for and consume the '='
    if it's in the context of a low precedence expression. The code that knows the current
    precedence is, logically enough, parsePrecedence() . The variable()
    function doesn’t need to know the actual level. It just cares that the precedence
    is low enough to allow assignment, so we pass that fact in as a Boolean.
    Since assignment is the lowest precedence expression, the only time we allow an
    assignment is when parsing an assignment expression or top-level expression
    like in an expression statement. */

    /* If the variable is nested inside some expression with
    higher precedence, canAssign will be false and this will ignore the = even
    if there is one there. Then this returns and eventually makes its way back to
    parsePrecedence(). */
    bool canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(canAssign);

    /* If the next token is too low precedence, or isn’t an infix operator at all, we’re
    done. We’ve parsed as much expression as we can. Otherwise, we consume the
    operator and hand off control to the infix parser we found. It consumes
    whatever other tokens it needs (usually the right operand) and returns back to
    parsePrecedence() . Then we loop back around and see if the next token is
    also a valid infix operator that can take the entire preceding expression as its
    operand. We keep looping like that, crunching through infix operators and their
    operands until we hit a token that isn’t an infix operator or is too low
    precedence and stop. */

    // (lower precedence things can contain higher precedence, ex. assignment can have infix
    // operators in it)
    while (precedence <= getRule(parser.current.type)->precedence) {
        advance();
        ParseFn infixRule = getRule(parser.previous.type)->infix;
        infixRule(canAssign);
    }

    if (canAssign && match(TOKEN_EQUAL)) {
        error("Invalid assignment target.");
    }
}

// Take the given token and add it's lexeme to the chunk's constant table as a string,
// return the index.
static uint8_t identifierConstant(Token* name) {
    return makeConstant(OBJ_VAL(copyString(name->start, name->length)));
}

static Token syntheticToken(const char* text) {
    Token token;
    token.start = text;
    token.length = (int)strlen(text);
    return token;
}

/* This is the point where the compiler records the existence of the variable. We
only do this for locals, so if we’re in the top level global scope, we just bail out.
Because global variables are late bound, the compiler doesn’t keep track of
which declarations for them it has seen. */
static void declareVariable() {
    // Global variables are implicitly declared.
    if (current->scopeDepth == 0) return;

    Token* name = &parser.previous;
    // Look through the local variable stack to ensure that variables
    // are not being redefined in the same scope.
    for (int i = current->localCount - 1; i >= 0; i--) {
        Local* local = &current->locals[i];

        // This is a stack, if top of stack has lower scope depth than new variable
        // to be inserted, then it is not a redefinition and we can break out early.
        if (local->depth != -1 && local->depth < current->scopeDepth) {
            break;
        }

        // If the variable was declared but not assigned to before (depth == -1)
        // and the scope depth and name matches, that is invalid.
        if (identifiersEqual(name, &local->name)) {
            error("Already variable with this name in this scope.");
        }
    }

    addLocal(*name);
}

static bool identifiersEqual(Token* a, Token* b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

static int resolveLocal(Compiler* compiler, Token* name) {
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (identifiersEqual(name, &local->name)) {
            if (local->depth == -1) {
                error("Can't read local variable in it's own initializer.");
            }
            return i;
        }
    }

    return -1;
}

static int resolveUpvalue(Compiler* compiler, Token* name) {
    // If we've reached the enclosing compiler and didn't find the local variable, it's global.
    if (compiler->enclosing == NULL) return -1;

    // Try to resolve the identifier as a local variable in the enclosing compiler.
    int local = resolveLocal(compiler->enclosing, name);
    if (local != -1) {
        compiler->enclosing->locals[local].isCaptured = true;
        return addUpvalue(compiler, (uint8_t)local, true);
    }

    /* Otherwise, we look for a local variable beyond the immediately enclosing
     * function. We do that by recursively calling resolveUpvalue() on the
     * enclosing compiler, not the current one. This series of resolveUpvalue() calls
     * works its way along the chain of nested compilers until it hits one of the base
     * cases—either it finds an actual local variable to capture or it runs out of
     * compilers. */
    int upvalue = resolveUpvalue(compiler->enclosing, name);
    if (upvalue != -1) {
        return addUpvalue(compiler, (uint8_t)upvalue, false);
    }

    return -1;
}

/* This creates a new Local and appends it to the compiler’s array of variables. It
stores the variable’s name and the depth of the scope that owns the variable. */
static void addLocal(Token name) {
    if (current->localCount == UINT8_COUNT) {
        error("Too many local variables in function.");
        return;
    }
    Local* local      = &current->locals[current->localCount++];
    local->name       = name;
    local->depth      = -1;
    local->isCaptured = false;
}

static int addUpvalue(Compiler* compiler, uint8_t index, bool isLocal) {
    int upvalueCount = compiler->function->upvalueCount;

    // If we find an upvalue in the array whose slot index matches the one we’re
    // adding, we just return that upvalue index and reuse it. Otherwise, we fall
    // through and add the new upvalue.
    for (int i = 0; i < upvalueCount; i++) {
        Upvalue* upvalue = &compiler->upvalues[i];
        if (upvalue->index == index && upvalue->isLocal == isLocal) {
            return i;
        }
    }

    if (upvalueCount == UINT8_COUNT) {
        error("Too many closure variables in function.");
        return 0;
    }

    compiler->upvalues[upvalueCount].isLocal = isLocal;
    compiler->upvalues[upvalueCount].index   = index;
    return compiler->function->upvalueCount++;
}

static void namedVariable(Token name, bool canAssign) {
    uint8_t getOp, setOp;
    int     arg = resolveLocal(current, &name);
    if (arg != -1) {
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
    } else if ((arg = resolveUpvalue(current, &name)) != -1) {
        getOp = OP_GET_UPVALUE;
        setOp = OP_SET_UPVALUE;
    } else {
        arg   = identifierConstant(&name);
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
    }

    if (canAssign && match(TOKEN_EQUAL)) {
        expression();
        emitBytes(setOp, (uint8_t)arg);
    } else {
        emitBytes(getOp, (uint8_t)arg);
    }
}

static void variable(bool canAssign) { namedVariable(parser.previous, canAssign); }

static void this_(bool canAssign) {
    if (currentClass == NULL) {
        error("Can't use 'this' outside of a class.");
        return;
    }
    variable(false);
}

static void and_(bool canAssign) {
    int endJump = emitJump(OP_JUMP_IF_FALSE);

    emitByte(OP_POP);  // Pop the left-hand side
    parsePrecedence(PREC_AND);

    // When the left-hand side of the and is falsey
    // that value sticks around to become the result
    // of the entire expression.
    patchJump(endJump);
}

static void or_(bool canAssign) {
    // In this, we need to jump if the left-hand side is truthy
    int avoidJump = emitJump(OP_JUMP_IF_FALSE);
    int endJump   = emitJump(OP_JUMP);

    patchJump(avoidJump);
    emitByte(OP_POP);  // Pop the left-hand side value
    // Right-hand side value will stick around to become
    // the result of the entire expression.

    parsePrecedence(PREC_OR);
    patchJump(endJump);
    // Otherwise skip the right-hand side and the
    // left-hand side value will stick around to become
    // the result of the entire expression.
}

static void super_(bool canAssign) {
    if (currentClass == NULL) {
        error("Can't use 'super' outside of a class.");
    } else if (!currentClass->hasSuperclass) {
        error("Can't use 'super' in a class with no superclass.");
    }

    consume(TOKEN_DOT, "Expect '.' after 'super'.");
    consume(TOKEN_IDENTIFIER, "Expect superclass method name.");
    uint8_t name = identifierConstant(&parser.previous);

    namedVariable(syntheticToken("this"), false);

    if (match(TOKEN_LEFT_PAREN)) {
        uint8_t argCount = argumentList();
        namedVariable(syntheticToken("super"), false);
        emitBytes(OP_SUPER_INVOKE, name);
        emitByte(argCount);
    } else {
        namedVariable(syntheticToken("super"), false);
        emitBytes(OP_GET_SUPER, name);
    }


    namedVariable(syntheticToken("super"), false);
    emitBytes(OP_GET_SUPER, name);
}

static uint8_t parseVariable(const char* errorMessage) {
    consume(TOKEN_IDENTIFIER, errorMessage);

    declareVariable();
    /* At runtime, locals aren’t looked up
    by name. There’s no need to stuff the variable’s name into the constant table so
    if the declaration is inside a local scope we return a dummy table index instead. */
    if (current->scopeDepth > 0) return 0;

    return identifierConstant(&parser.previous);
}

static void defineVariable(uint8_t global) {
    if (current->scopeDepth > 0) {
        markInitialized();
        return;
    }
    emitBytes(OP_DEFINE_GLOBAL, global);
}

static uint8_t argumentList() {
    uint8_t argCount = 0;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            expression();
            if (argCount == 255) {
                error("Can't have more than 255 arguments.");
            }
            argCount++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
    return argCount;
}

static void markInitialized() {
    if (current->scopeDepth == 0) return;
    current->locals[current->localCount - 1].depth = current->scopeDepth;
}

static void expression() { parsePrecedence(PREC_ASSIGNMENT); }

static void varDeclaration() {
    uint8_t slot = parseVariable("Expect variable name.");

    if (match(TOKEN_EQUAL)) {
        expression();
    } else {
        emitByte(OP_NIL);
    }
    consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

    defineVariable(slot);
}

static void funDeclaration() {
    uint8_t global = parseVariable("Expect function name.");
    markInitialized();
    function(TYPE_FUNCTION);
    defineVariable(global);
}

static void expressionStatement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
    emitByte(OP_POP);
}

static void printStatement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after value.");
    emitByte(OP_PRINT);
}

static void returnStatement() {
    if (current->type == TYPE_SCRIPT) {
        error("Can't return from top-level code.");
    }

    if (match(TOKEN_SEMICOLON))
        emitReturn();
    else {
        if (current->type == TYPE_INITIALIZER) {
            error("Can't return a value from an initializer.");
        }

        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
        emitByte(OP_RETURN);
    }
}

static void synchronize() {
    parser.panicMode = false;

    while (parser.current.type != TOKEN_EOF) {
        if (parser.previous.type == TOKEN_SEMICOLON) return;

        switch (parser.current.type) {
            case TOKEN_CLASS:
            case TOKEN_FUN:
            case TOKEN_VAR:
            case TOKEN_FOR:
            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_PRINT:
            case TOKEN_RETURN: return;
            default:  // Do nothing.
                break;
        }

        advance();
    }
}

static void declaration() {
    if (match(TOKEN_CLASS)) {
        classDeclaration();
    } else if (match(TOKEN_FUN)) {
        funDeclaration();
    } else if (match(TOKEN_VAR)) {
        varDeclaration();
    } else {
        statement();
    }

    if (parser.panicMode) synchronize();
}

static void block() {
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        declaration();
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void function(FunctionType type) {
    Compiler compiler;
    initCompiler(&compiler, type);
    beginScope();

    // Compile the parameter list.
    consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            current->function->arity++;
            if (current->function->arity > 255) {
                errorAtCurrent("Can't have more than 255 parameters.");
            }
            uint8_t paramConstant = parseVariable("Expect parameter name.");
            defineVariable(paramConstant);
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");

    // The body.
    consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
    block();

    ObjFunction* function = endCompiler();
    emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(function)));

    for (int i = 0; i < function->upvalueCount; i++) {
        emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
        emitByte(compiler.upvalues[i].index);
    }
}

static void method() {
    consume(TOKEN_IDENTIFIER, "Expect method name.");
    uint8_t constant = identifierConstant(&parser.previous);

    FunctionType type = TYPE_METHOD;
    if (parser.previous.length == 4 && memcmp(parser.previous.start, "init", 4) == 0) {
        type = TYPE_INITIALIZER;
    }

    function(type);
    emitBytes(OP_METHOD, constant);
}

static void classDeclaration() {
    consume(TOKEN_IDENTIFIER, "Expect class name.");
    Token   className    = parser.previous;
    uint8_t nameConstant = identifierConstant(&parser.previous);
    declareVariable();

    emitBytes(OP_CLASS, nameConstant);
    defineVariable(nameConstant);

    ClassCompiler classCompiler;

    classCompiler.name      = parser.previous;
    classCompiler.hasSuperclass = false;
    classCompiler.enclosing = currentClass;
    currentClass            = &classCompiler;

    if (match(TOKEN_LESS)) {
        consume(TOKEN_IDENTIFIER, "Expect superclass name.");
        variable(false); // Look up superclass by name and put on the stack.

        if (identifiersEqual(&className, &parser.previous)) {
            error("A class can't inherit from itself.");
        }

        // Over in the front end, compiling the superclass clause emits bytecode that loads
        // the superclass onto the stack. Instead of leaving that slot as a temporary, we
        // create a new scope and make it a local variable.

        // Creating a new lexical scope ensures that if we declare two classes in the same
        // scope, each has a different local slot to store its superclass. Since we always
        // name this variable “super”, if we didn’t make a scope for each subclass, the
        // variables would collide.
        beginScope();
        addLocal(syntheticToken("super"));
        defineVariable(0);
    
        namedVariable(className, false); // Load subclass doing the inheriting onto the stack.
        emitByte(OP_INHERIT);
        classCompiler.hasSuperclass = true;
    }

    namedVariable(className, false);
    consume(TOKEN_LEFT_BRACE, "Expect '{' before class body.");
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        method();
    }
    consume(TOKEN_RIGHT_BRACE, "Expect '}' after class body.");
    emitByte(OP_POP);

    if (classCompiler.hasSuperclass) {
        endScope();
    }

    currentClass = currentClass->enclosing;
}

static void ifStatement() {
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    int thenJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);  // Pop the condition inside the if branch.
    statement();

    int elseJump = emitJump(OP_JUMP);

    patchJump(thenJump);
    emitByte(OP_POP);  // Pop the condition inside the else branch.

    if (match(TOKEN_ELSE)) statement();
    patchJump(elseJump);
}

static void whileStatment() {
    int loopStart = currentChunk()->count;

    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    int exitJump = emitJump(OP_JUMP_IF_FALSE);

    emitByte(OP_POP);
    statement();

    emitLoop(loopStart);

    patchJump(exitJump);
    emitByte(OP_POP);
}

static void forStatement() {
    beginScope();

    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");

    // In a for loop, first the initializer is run once (and only once)
    if (match(TOKEN_SEMICOLON)) {
        // No initializer.
    } else if (match(TOKEN_VAR))
        varDeclaration();
    else {
        expressionStatement();
    }

    int loopStart = currentChunk()->count;

    // The condition is checked before every run of the loop.
    int exitJump = -1;
    if (!match(TOKEN_SEMICOLON)) {
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

        // Jump out of the loop if the condition is false.
        exitJump = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP);  // Condition
    }

    // When an increment is present, we need to compile it now, but it shouldn't execute yet.
    if (!match(TOKEN_RIGHT_PAREN)) {
        // We emit an unconditional jump that hops over the increment clause to the body of the
        // loop.
        int bodyJump = emitJump(OP_JUMP);

        int incrementStart = currentChunk()->count;
        expression();      // Next we compile the increment expression itself.
        emitByte(OP_POP);  // Pop expression, since it is only executed for it's side effect.
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after for loop initializer.");

        emitLoop(loopStart);  // First, we emit a loop instruction.
        // Then we change loopStart to point to the offset where the increment expression begins.
        // This is how we stitch the increment in to run after the body.
        loopStart = incrementStart;
        patchJump(bodyJump);
    }

    statement();
    emitLoop(loopStart);

    if (exitJump != -1) {
        patchJump(exitJump);
        emitByte(OP_POP);  // Condition.
    }

    endScope();
}

static void statement() {
    if (match(TOKEN_PRINT)) {
        printStatement();
    } else if (match(TOKEN_IF)) {
        ifStatement();
    } else if (match(TOKEN_RETURN)) {
        returnStatement();
    } else if (match(TOKEN_WHILE)) {
        whileStatment();
    } else if (match(TOKEN_FOR)) {
        forStatement();
    } else if (match(TOKEN_LEFT_BRACE)) {
        beginScope();
        block();
        endScope();
    } else
        expressionStatement();
}

ParseRule rules[] = {
    [TOKEN_LEFT_PAREN]    = {grouping, call,   PREC_CALL      },
    [TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE      },
    [TOKEN_LEFT_BRACE]    = {NULL,     NULL,   PREC_NONE      },
    [TOKEN_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE      },
    [TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE      },
    [TOKEN_DOT]           = {NULL,     dot,    PREC_CALL      },
    [TOKEN_MINUS]         = {unary,    binary, PREC_TERM      },
    [TOKEN_PLUS]          = {NULL,     binary, PREC_TERM      },
    [TOKEN_SEMICOLON]     = {NULL,     NULL,   PREC_NONE      },
    [TOKEN_SLASH]         = {NULL,     binary, PREC_FACTOR    },
    [TOKEN_STAR]          = {NULL,     binary, PREC_FACTOR    },
    [TOKEN_BANG]          = {unary,    NULL,   PREC_NONE      },
    [TOKEN_BANG_EQUAL]    = {NULL,     binary, PREC_EQUALITY  },
    [TOKEN_EQUAL]         = {NULL,     NULL,   PREC_NONE      },
    [TOKEN_EQUAL_EQUAL]   = {NULL,     binary, PREC_EQUALITY  },
    [TOKEN_GREATER]       = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL] = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_LESS]          = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_LESS_EQUAL]    = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_IDENTIFIER]    = {variable, NULL,   PREC_NONE      },
    [TOKEN_STRING]        = {string,   NULL,   PREC_NONE      },
    [TOKEN_NUMBER]        = {number,   NULL,   PREC_NONE      },
    [TOKEN_AND]           = {NULL,     and_,   PREC_AND       },
    [TOKEN_CLASS]         = {NULL,     NULL,   PREC_NONE      },
    [TOKEN_ELSE]          = {NULL,     NULL,   PREC_NONE      },
    [TOKEN_FALSE]         = {literal,  NULL,   PREC_NONE      },
    [TOKEN_FOR]           = {NULL,     NULL,   PREC_NONE      },
    [TOKEN_FUN]           = {NULL,     NULL,   PREC_NONE      },
    [TOKEN_IF]            = {NULL,     NULL,   PREC_NONE      },
    [TOKEN_NIL]           = {literal,  NULL,   PREC_NONE      },
    [TOKEN_OR]            = {NULL,     or_,    PREC_OR        },
    [TOKEN_PRINT]         = {NULL,     NULL,   PREC_NONE      },
    [TOKEN_RETURN]        = {NULL,     NULL,   PREC_NONE      },
    [TOKEN_SUPER]         = {super_,     NULL,   PREC_NONE      },
    [TOKEN_THIS]          = {this_,    NULL,   PREC_NONE      },
    [TOKEN_TRUE]          = {literal,  NULL,   PREC_NONE      },
    [TOKEN_VAR]           = {NULL,     NULL,   PREC_NONE      },
    [TOKEN_WHILE]         = {NULL,     NULL,   PREC_NONE      },
    [TOKEN_ERROR]         = {NULL,     NULL,   PREC_NONE      },
    [TOKEN_EOF]           = {NULL,     NULL,   PREC_NONE      },
};

static ParseRule* getRule(TokenType operatorType) { return &rules[operatorType]; }

ObjFunction* compile(const char* source) {
    initScanner(source);

    Compiler compiler;
    initCompiler(&compiler, TYPE_SCRIPT);

    parser.hadError  = false;
    parser.panicMode = false;

    advance();

    while (!match(TOKEN_EOF)) {
        declaration();
    }

    ObjFunction* function = endCompiler();
    return parser.hadError ? NULL : function;
}

void markCompilerRoots() {
    Compiler* compiler = current;
    while (compiler != NULL) {
        markObject((Obj*)compiler->function);
        compiler = compiler->enclosing;
    }
}