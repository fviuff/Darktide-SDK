#pragma once
// Reader for the game's compiled shaders: DXBC containers holding DXIL (LLVM 3.7 bitcode, shader model 6.0).
//
// parse_container() splits a container into its parts; Module reads a DXIL program (the DXIL part for the code, the
// STAT part for the names the shader was compiled with) into types, constants, globals, metadata and the
// instructions of each function, numbered the way LLVM numbers values.
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dtglb::stingray::shader {

std::map<std::string, std::vector<std::uint8_t>> parse_container(const std::vector<std::uint8_t>& data, std::string& error);
// the bitcode of a DXIL / STAT part and its shader kind (0 pixel, 1 vertex, ...)
bool program_bitcode(const std::vector<std::uint8_t>& part, std::uint32_t& kind, std::vector<std::uint8_t>& bitcode,
                     std::string& error);

struct Type {
    enum Kind { Void, Float, Half, Double, Label, Metadata, Int, Pointer, Array, Vector, Struct, Function, Unknown } kind = Unknown;
    std::uint32_t width = 0;           // Int
    std::uint64_t count = 0;           // Array, Vector
    const Type* element = nullptr;     // Array, Vector
    const Type* pointee = nullptr;     // Pointer
    std::vector<const Type*> members;  // Struct
    std::string name;                  // Struct
    const Type* ret = nullptr;         // Function
    std::vector<const Type*> params;   // Function
    bool is_float() const { return kind == Float || kind == Half || kind == Double; }
};

struct Instruction;
struct Function;

struct Value {
    enum Kind { Const, Undef, Null, Aggregate, Data, Expr, Global, FunctionRef, Argument, InstructionResult, Forward } kind = Const;
    const Type* type = nullptr;
    std::int64_t int_value = 0;    // Const (ints), Null
    double float_value = 0;        // Const (floats)
    std::uint64_t bits = 0;        // Const (floats): the stored bits
    std::vector<const Value*> elements;  // Aggregate
    std::vector<std::uint64_t> raw_elements;
    std::vector<double> float_items;     // Data (float/double elements)
    std::vector<std::uint64_t> items;    // Data (integer elements)
    // Global
    const Type* value_type = nullptr;
    std::uint64_t init_id = 0;
    const Value* init = nullptr;
    Function* function = nullptr;   // FunctionRef
    Instruction* instruction = nullptr;  // InstructionResult
    std::size_t index = 0;          // value number
    std::size_t forward_id = 0;     // Forward
    bool is_constant() const { return kind == Const || kind == Null; }
    std::int64_t constant() const { return kind == Const ? int_value : 0; }
};

struct Instruction {
    enum Op { Binop, Cast, Select, ExtractValue, InsertValue, ExtractElement, InsertElement, Cmp, Ret, Br, Switch,
              Unreachable, Phi, Alloca, Load, Store, Gep, Call, Fence, AtomicRmw, CmpXchg } op = Ret;
    const Type* type = nullptr;  // result type (null: no value)
    std::vector<const Value*> operands;
    const Value* value = nullptr;
    std::uint32_t opcode = 0;     // Binop: add sub mul udiv sdiv urem srem shl lshr ashr and or xor; Cast: trunc ... addrspacecast
    std::vector<std::uint64_t> indices;   // ExtractValue, InsertValue
    std::vector<std::uint32_t> targets;   // Br
    std::uint32_t predicate = 0;          // Cmp
    std::uint32_t default_target = 0;     // Switch
    std::vector<std::pair<const Value*, std::uint32_t>> cases;  // Switch
    std::vector<std::uint32_t> blocks;    // Phi: incoming blocks, with operands
    const Value* callee = nullptr;        // Call
    const Type* allocated = nullptr;      // Alloca
    const Type* source = nullptr;         // Gep
};

struct Function {
    const Type* type = nullptr;
    bool prototype = false;
    const Value* value = nullptr;
    std::vector<std::vector<Instruction*>> blocks;
};

// a resolved metadata node
struct Metadata {
    enum Kind { None, String, ValueRef, Node } kind = None;
    std::string string;
    const Value* value = nullptr;
    std::vector<Metadata> items;
    std::int64_t number() const { return kind == ValueRef && value && value->kind == Value::Const ? value->int_value : 0; }
    bool is_null() const { return kind == None; }
};

class Module {
public:
    bool read(const std::vector<std::uint8_t>& bitcode, std::string& error);

    std::vector<Function*> functions;
    std::map<std::string, std::vector<std::uint64_t>> named_metadata;
    Metadata md(std::uint64_t index, int depth = 0) const;

private:
    friend struct ModuleReader;
    std::deque<Type> type_storage_;
    std::vector<const Type*> types_;
    std::deque<Value> value_storage_;
    std::deque<Instruction> instruction_storage_;
    std::deque<Function> function_storage_;
    std::vector<const Value*> values_;
    std::vector<Function*> bodies_;
    struct RawMetadata {
        int kind = 0;  // 1 string, 2 value, 3 node
        std::string string;
        std::uint64_t value = 0;
        std::vector<std::int64_t> node;  // -1 = null
    };
    std::map<std::uint64_t, RawMetadata> metadata_;
    std::uint64_t metadata_count_ = 0;
    std::uint64_t version_ = 0;
};

}  // namespace dtglb::stingray::shader
