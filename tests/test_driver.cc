
#include <rellume/rellume.h>

#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/mman.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>


struct HexBuffer {
    uint8_t* buf;
    size_t size;
    friend std::ostream& operator<<(std::ostream& os, HexBuffer const& self) {
        os << std::hex << std::setfill('0');
        for (size_t i = 0; i != self.size; i++)
            os << std::setw(2) << static_cast<int>(self.buf[i]);
        return os << std::dec;
    }
};

struct CPU {
    uint8_t rip[8];
    uint8_t gpr[8][16];
    uint8_t flags[6];
    uint8_t _pad[2];
    uint8_t sse[16][16];
} __attribute__((packed));

struct RegEntry {
    size_t size;
    off_t offset;
};

static std::unordered_map<std::string,RegEntry> regs = {
    {"rip", {8, offsetof(CPU, rip)}},
    {"rax", {8, offsetof(CPU, gpr[0])}},
    {"zf", {1, offsetof(CPU, flags[0])}},
    {"sf", {1, offsetof(CPU, flags[1])}},
    {"pf", {1, offsetof(CPU, flags[2])}},
    {"cf", {1, offsetof(CPU, flags[3])}},
    {"of", {1, offsetof(CPU, flags[4])}},
    {"af", {1, offsetof(CPU, flags[5])}},
};

class TestCase {


    std::ostringstream& diagnostic;
    std::vector<std::pair<void*, size_t>> mem_maps;

    TestCase(std::ostringstream& diagnostic) : diagnostic(diagnostic) {}

    ~TestCase() {
        for (auto& map : mem_maps) {
            munmap(map.first, map.second);
        }
    }

    bool SetReg(std::string reg, std::string value_str, CPU* cpu) {
        auto reg_entry = regs.find(reg);
        if (reg_entry == regs.end()) {
            diagnostic << "# invalid register: " << reg << std::endl;
            return true;
        }

        if (value_str.length() != reg_entry->second.size * 2) {
            diagnostic << "# invalid input length: " << value_str << std::endl;
            return true;
        }

        uint8_t* cpu_raw = reinterpret_cast<uint8_t*>(cpu);
        uint8_t* buf = cpu_raw + reg_entry->second.offset;
        for (size_t i = 0; i < reg_entry->second.size; i++) {
            char hex_byte[3] = {value_str[i*2],value_str[i*2+1], 0};
            buf[i] = std::strtoul(hex_byte, nullptr, 16);
        }

        return false;
    }

    bool AllocMem(std::string key, std::string value_str) {
        uintptr_t addr = std::stoul(key.substr(1), nullptr, 16);
        size_t value_len = value_str.length() / 2;

        void* map = mmap(reinterpret_cast<void*>(addr), value_len,
                         PROT_READ|PROT_WRITE,
                         MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
        if (map == MAP_FAILED) {
            diagnostic << "# error mapping address " << std::hex << addr << std::endl;
            return true;
        }
        mem_maps.push_back(std::make_pair(map, value_len));

        uint8_t* buf = reinterpret_cast<uint8_t*>(map);
        for (size_t i = 0; i < value_len; i++) {
            char hex_byte[3] = {value_str[i*2],value_str[i*2+1], 0};
            buf[i] = std::strtoul(hex_byte, nullptr, 16);
        }

        return false;
    }

    bool CheckMem(std::string key, std::string value_str) {
        uintptr_t addr = std::stoul(key.substr(1), nullptr, 16);
        size_t value_len = value_str.length() / 2;

        uint8_t* buf = reinterpret_cast<uint8_t*>(addr);
        bool fail = false;
        for (size_t i = 0; i < value_len; i++) {
            char hex_byte[3] = {value_str[i*2],value_str[i*2+1], 0};
            uint8_t val = std::strtoul(hex_byte, nullptr, 16);
            if (buf[i] != val) {
                fail = true;
                diagnostic << "# unexpected value for " << std::hex << addr << std::endl;
                diagnostic << "# expected: " << HexBuffer{&val, 1} << std::endl;
                diagnostic << "#      got: " << HexBuffer{&buf[i], 1} << std::endl;
            }
        }

        return fail;
    }

    std::pair<std::string, std::string> split_arg(std::string arg) {
        size_t value_off = arg.find('=');
        if (value_off == std::string::npos) {
            std::cerr << "invalid input: " << arg << std::endl;
            std::exit(1);
        }

        std::string key_str = arg.substr(0, value_off);
        std::string value_str = arg.substr(value_off + 1);
        return std::make_pair(key_str, value_str);
    }

    bool Run(std::string argstring) {
        std::istringstream argstream(argstring);
        std::string arg;
        bool fail = false;

        // 1. Setup initial state
        CPU initial{};

        while (argstream >> arg) {
            if (arg == "=>")
                goto run_function;

            auto kv = split_arg(arg);
            if (kv.first[0] == 'm') {
                AllocMem(kv.first, kv.second);
            } else {
                SetReg(kv.first, kv.second, &initial);
            }
        }

        // We didn't run anything.
        diagnostic << "# error: no emulation command" << std::endl;
        return true;

    run_function:

        // 2. Emulate function
        CPU state = initial;

        llvm::LLVMContext ctx;
        auto mod = std::make_unique<llvm::Module>("rellume_test", ctx);

        LLFunc* rlfn = ll_func(llvm::wrap(mod.get()));
        ll_func_decode(rlfn, *reinterpret_cast<uint64_t*>(&state.rip));
        llvm::Function* fn = llvm::unwrap<llvm::Function>(ll_func_lift(rlfn));
        ll_func_dispose(rlfn);
        if (fn == nullptr) {
            diagnostic << "# error during lifting" << std::endl;
            return true;
        }

        fn->setName("test_function");
        // fn->print(llvm::errs());

        std::string error;

        llvm::EngineBuilder builder(std::move(mod));
        builder.setEngineKind(llvm::EngineKind::JIT);
        builder.setErrorStr(&error);

        llvm::SmallVector<std::string, 1> MAttrs;
        llvm::Triple triple = llvm::Triple(llvm::sys::getProcessTriple());
        llvm::TargetMachine* target = builder.selectTarget(triple, "x86-64", llvm::sys::getHostCPUName(), MAttrs);

        if (llvm::ExecutionEngine* engine = builder.create(target)) {
            auto fn_ptr = reinterpret_cast<void(*)(CPU*)>(engine->getFunctionAddress(fn->getName()));
            fn_ptr(&state);
            delete engine;
        } else {
            diagnostic << "# error creating engine: " << error << std::endl;
            return true;
        }

        // 3. Compare with expected values
        //  - memory is compared immediately
        //  - registers are compared separately to support undefined values
        CPU expected = initial;

        std::unordered_set<std::string> skip_regs;
        while (argstream >> arg) {
            auto kv = split_arg(arg);
            if (kv.first[0] == 'm') {
                fail |= CheckMem(kv.first, kv.second);
            } else if (kv.second == "undef") {
                skip_regs.insert(kv.first);
            } else {
                SetReg(kv.first, kv.second, &expected);
            }
        }

        uint8_t* state_raw = reinterpret_cast<uint8_t*>(&state);
        uint8_t* expected_raw = reinterpret_cast<uint8_t*>(&expected);
        for (auto& reg_entry : regs) {
            if (skip_regs.count(reg_entry.first) > 0)
                continue;

            size_t size = reg_entry.second.size;
            size_t offset = reg_entry.second.offset;
            uint8_t* expected = expected_raw + offset;
            uint8_t* state = state_raw + offset;
            if (memcmp(state, expected, size) != 0) {
                fail = true;
                diagnostic << "# unexpected value for " << reg_entry.first << std::endl;
                diagnostic << "# expected: " << HexBuffer{expected, size} << std::endl;
                diagnostic << "#      got: " << HexBuffer{state, size} << std::endl;
            }
        }

        return fail;
    }

public:
    static bool Run(unsigned number, std::string caseline) {
        std::ostringstream diagnostic;
        TestCase test_case(diagnostic);
        bool fail = test_case.Run(caseline);
        if (fail)
            std::cout << "not ";
        std::cout << "ok " << number << " " << caseline << std::endl;
        std::cout << diagnostic.str();
        return fail;
    }
};

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " casefile" << std::endl;
        return 1;
    }

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    std::ifstream casefile(argv[1]);
    if (casefile.fail()) {
        std::cerr << "error opening casefile" << std::endl;
        return 1;
    }

    unsigned count = 0;
    bool fail = false;
    for (std::string caseline; std::getline(casefile, caseline); count++)
        fail |= TestCase::Run(count + 1, caseline);

    std::cout << "1.." << count << std::endl;

    return fail ? 1 : 0;
}
