#include "runtime_utils.h"
#include "vm.h"
#include "table_object.h"
#include "deegen_enter_vm_from_c.h"

#include "generated/get_guest_language_function_interpreter_entry_point.h"
#include "json_utils.h"

TValue WARN_UNUSED MakeErrorMessage(const char* msg)
{
    return TValue::CreatePointer(VM::GetActiveVMForCurrentThread()->CreateStringObjectFromRawString(msg, static_cast<uint32_t>(strlen(msg))));
}

TValue WARN_UNUSED MakeErrorMessageForUnableToCall(TValue badValue)
{
    // The Lua message is "attmpt to call a (type of badValue) value"
    //
    char msg[100];
    auto makeMsg = [&](const char* ty)
    {
        snprintf(msg, 100, "attempt to call a %s value", ty);
    };
    auto makeMsg2 = [&](int d)
    {
        snprintf(msg, 100, "attempt to call a (internal type %d) value", d);
    };

    if (badValue.IsInt32())
    {
        makeMsg("number");
    }
    else if (badValue.IsDouble())
    {
        makeMsg("number");
    }
    else if (badValue.IsMIV())
    {
        MiscImmediateValue miv = badValue.AsMIV();
        if (miv.IsNil())
        {
            makeMsg("nil");
        }
        else
        {
            assert(miv.IsBoolean());
            makeMsg("boolean");
        }
    }
    else
    {
        assert(badValue.IsPointer());
        UserHeapGcObjectHeader* p = TranslateToRawPointer(badValue.AsPointer<UserHeapGcObjectHeader>().As());
        if (p->m_type == HeapEntityType::String)
        {
            makeMsg("string");
        }
        else if (p->m_type == HeapEntityType::Function)
        {
            makeMsg("function");
        }
        else if (p->m_type == HeapEntityType::Table)
        {
            makeMsg("table");
        }
        else if (p->m_type == HeapEntityType::Thread)
        {
            makeMsg("thread");
        }
        else
        {
            // TODO: handle userdata type
            //
            makeMsg2(static_cast<int>(p->m_type));
        }
    }
    return MakeErrorMessage(msg);
}

CodeBlock* WARN_UNUSED CodeBlock::Create(VM* vm, UnlinkedCodeBlock* ucb, UserHeapPointer<TableObject> globalObject)
{
    size_t sizeToAllocate = GetTrailingArrayOffset() + RoundUpToMultipleOf<8>(ucb->m_bytecodeMetadataLength) + sizeof(TValue) * ucb->m_cstTableLength;
    uint8_t* addressBegin = TranslateToRawPointer(vm, vm->AllocFromSystemHeap(static_cast<uint32_t>(sizeToAllocate)).AsNoAssert<uint8_t>());
    memcpy(addressBegin, ucb->m_cstTable, sizeof(TValue) * ucb->m_cstTableLength);

    CodeBlock* cb = reinterpret_cast<CodeBlock*>(addressBegin + sizeof(TValue) * ucb->m_cstTableLength);
    SystemHeapGcObjectHeader::Populate<ExecutableCode*>(cb);
    cb->m_hasVariadicArguments = ucb->m_hasVariadicArguments;
    cb->m_numFixedArguments = ucb->m_numFixedArguments;
    cb->m_bytecode = new uint8_t[ucb->m_bytecodeLength];
    memcpy(cb->m_bytecode, ucb->m_bytecode, ucb->m_bytecodeLength);
    cb->m_bestEntryPoint = generated::GetGuestLanguageFunctionEntryPointForInterpreter(ucb->m_hasVariadicArguments, ucb->m_numFixedArguments);
    cb->m_globalObject = globalObject;
    cb->m_stackFrameNumSlots = ucb->m_stackFrameNumSlots;
    cb->m_numUpvalues = ucb->m_numUpvalues;
    cb->m_bytecodeLength = ucb->m_bytecodeLength;
    cb->m_bytecodeMetadataLength = ucb->m_bytecodeMetadataLength;
    cb->m_baselineCodeBlock = nullptr;
    cb->m_floCodeBlock = nullptr;
    cb->m_owner = ucb;
    return cb;
}

std::pair<TValue* /*retStart*/, uint64_t /*numRet*/> VM::LaunchScript(ScriptModule* module)
{
    CoroutineRuntimeContext* rc = GetRootCoroutine();
    return DeegenEnterVMFromC(rc, module->m_defaultEntryPoint.As(), rc->m_stackBegin);
}

UserHeapPointer<FunctionObject> WARN_UNUSED NO_INLINE FunctionObject::CreateAndFillUpvalues(CodeBlock* cb, CoroutineRuntimeContext* rc, TValue* stackFrameBase, HeapPtr<FunctionObject> parent)
{
    UnlinkedCodeBlock* ucb = cb->m_owner;
    HeapPtr<FunctionObject> r = Create(VM::GetActiveVMForCurrentThread(), cb).As();
    assert(TranslateToRawPointer(TCGet(parent->m_executable).As())->IsBytecodeFunction());
    assert(cb->m_owner->m_parent == static_cast<HeapPtr<CodeBlock>>(TCGet(parent->m_executable).As())->m_owner);
    uint32_t numUpvalues = cb->m_numUpvalues;
    UpvalueMetadata* upvalueInfo = ucb->m_upvalueInfo;
    for (uint32_t ord = 0; ord < numUpvalues; ord++)
    {
        UpvalueMetadata& uvmt = upvalueInfo[ord];
        GeneralHeapPointer<Upvalue> uv;
        if (uvmt.m_isParentLocal)
        {
            uv = Upvalue::Create(rc, stackFrameBase + uvmt.m_slot, uvmt.m_isImmutable);
        }
        else
        {
            uv = GetUpvalue(parent, uvmt.m_slot);
        }
        TCSet(r->m_upvalues[ord], uv);
    }
    return r;
}