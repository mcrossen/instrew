
#include <instrew-api.h>

#include <llvm/IR/CallSite.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <cstdio>
#include <vector>

namespace {

class Instrumenter {
private:
    llvm::Function* instr_marker;

public:
    Instrumenter(const char* config, llvm::Module* mod,
                 InstrewDesc* out_desc) {
        instr_marker = mod->getFunction("instrew_instr_marker");

        // Fields instrument and finalize are filled out below.
        out_desc->magic = 0xAEDB1000;
        out_desc->flags = INSTREW_DESC_OPTIMIZE | INSTREW_DESC_SUPPORTS_HHVM |
                          INSTREW_DESC_CACHABLE | INSTREW_DESC_MARK_INSTRS;
        out_desc->name = "InstrCount";
        out_desc->uuid = "3ffc0f58-3a8f-4fa4-a14f-ec987f2a8e8c";

        (void) config;
    }

    ~Instrumenter() {}

    llvm::Function* Instrument(llvm::Function* fn) {
        llvm::BasicBlock& entry_block = fn->getEntryBlock();
        llvm::IRBuilder<> irb(&entry_block, entry_block.getFirstInsertionPt());

        // Get pointer to CPU struct from arguments.
        unsigned sptr_idx = 0;
        if (fn->getCallingConv() == llvm::CallingConv::HHVM)
            sptr_idx = 1;
        llvm::Value* sptr = &fn->arg_begin()[sptr_idx];

        // Construct pointer to "instruction counter" in entry block.
        unsigned sptr_as = sptr->getType()->getPointerAddressSpace();
        llvm::Type* count_ptr_ty = irb.getInt64Ty()->getPointerTo(sptr_as);
        llvm::Value* count_ptr = irb.CreateConstGEP1_64(sptr, -0x40);
        count_ptr = irb.CreatePointerCast(count_ptr, count_ptr_ty);

        // We can't remove instructions when iterating, so store call sites.
        std::vector<llvm::CallSite> call_sites;
        call_sites.reserve(instr_marker->getNumUses());

        for (llvm::BasicBlock& bb : *fn) {
            // Count all calls to the instruction marker
            uint64_t instr_count = 0;
            for (llvm::Instruction& instr : bb) {
                if (auto call = llvm::dyn_cast<llvm::CallInst>(&instr)) {
                    if (call->getCalledFunction() == instr_marker) {
                        instr_count++;
                        call_sites.emplace_back(call);
                    }
                }
            }

            // Update counter if the basic block contains some instructions.
            // Note that this therefore doesn't apply to the entry and exit
            // basic blocks.
            if (instr_count > 0) {
                irb.SetInsertPoint(bb.getFirstNonPHI());
                llvm::Value* count_ld = irb.CreateLoad(count_ptr);
                llvm::Value* offset = irb.getInt64(instr_count);
                llvm::Value* count_new = irb.CreateAdd(count_ld, offset);
                irb.CreateStore(count_new, count_ptr);
            }
        }

        // Remove all calls to the instruction marker.
        for (auto& cs : call_sites)
            cs.getInstruction()->eraseFromParent();

        return fn;
    }
};

} // anonymous namespace

void* instrew_init_instrumenter(const char* config, LLVMModuleRef mod,
                                struct InstrewDesc* out_desc) {
    auto* cls = new Instrumenter(config, llvm::unwrap(mod), out_desc);
    out_desc->instrument = [](void* handle, LLVMValueRef fn) {
        auto* cls = static_cast<Instrumenter*>(handle);
        return llvm::wrap(cls->Instrument(llvm::unwrap<llvm::Function>(fn)));
    };
    out_desc->finalize = [](void* handle) {
        delete static_cast<Instrumenter*>(handle);
    };
    return cls;
}
