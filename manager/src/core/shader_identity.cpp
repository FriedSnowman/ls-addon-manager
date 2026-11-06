#include "shader_identity.h"
#include "hook_util.h"
#include "eam/shader_identity.h"
#include <MinHook.h>
#include <bcrypt.h>
#include <mutex>
#include <utility>
#pragma comment(lib,"bcrypt.lib")

namespace ShaderIdentity {
namespace {
using CreateFn=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11ComputeShader**);
struct Hook { void* target=nullptr; CreateFn original=nullptr; };
constexpr size_t kCount=8;
Hook hooks[kCount];
std::mutex mutex;
size_t count=0;
template<size_t I> HRESULT STDMETHODCALLTYPE Create(ID3D11Device* device,const void* code,SIZE_T size,ID3D11ClassLinkage* linkage,ID3D11ComputeShader** out) {
    const HRESULT result=hooks[I].original(device,code,size,linkage,out);
    if(SUCCEEDED(result)&&out&&*out&&code&&size<=UINT_MAX) {
        eam::ShaderSha256 hash{};
        if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,(PUCHAR)code,static_cast<ULONG>(size),hash.data(),static_cast<ULONG>(hash.size()))>=0)
            (*out)->SetPrivateData(eam::kShaderSha256,static_cast<UINT>(hash.size()),hash.data());
    }
    return result;
}
template<size_t... I> void* Detour(size_t i,std::index_sequence<I...>) { void* functions[]={reinterpret_cast<void*>(&Create<I>)...}; return functions[i]; }
}
bool Attach(ID3D11Device* device) {
    if(!device)return false;
    std::lock_guard<std::mutex> lock(mutex);
    void* target=(*reinterpret_cast<void***>(device))[18];
    for(size_t i=0;i<count;++i)if(hooks[i].target==target)return true;
    if(count==kCount || !eam::hooks::Begin())return false;
    auto& h=hooks[count];
    if(MH_CreateHook(target,Detour(count,std::make_index_sequence<kCount>{}),reinterpret_cast<void**>(&h.original))!=MH_OK) { eam::hooks::End(); return false; }
    if(MH_EnableHook(target)!=MH_OK) { MH_RemoveHook(target); eam::hooks::End(); return false; }
    h.target=target;++count;
    // Same lifetime as the existing Dispatch detours: the runtime may chain through them.
    return true;
}
}
