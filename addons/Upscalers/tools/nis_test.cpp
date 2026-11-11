// Real D3D11 bindings with synthetic shaders: no LS install or vendor runtime needed.
#include "addon/scaler11.h"
#include <eam/shader_identity.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
namespace {
void Require(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D11 fixture failed"); }
struct Texture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    Texture(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format) {
        D3D11_TEXTURE2D_DESC d{}; d.Width=w; d.Height=h; d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=format; d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        Require(device->CreateTexture2D(&d,nullptr,&texture));
        Require(device->CreateShaderResourceView(texture.Get(),nullptr,&srv));
        Require(device->CreateUnorderedAccessView(texture.Get(),nullptr,&uav));
    }
};
struct Constants { float f[18]{}; UINT inX=0,inY=0,inW=320,inH=240,outX=128,outY=96,outW=384,outH=288; UINT pad[2]{}; };
}
int main() {
    unsigned failures=0;
    auto check=[&](const char* name,bool ok){printf("%s %s\n",ok?"PASS":"FAIL",name);if(!ok)++failures;};
    try {
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        Require(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        ComPtr<ID3DBlob> code;
        const char source[]="[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) {}";
        Require(D3DCompile(source,sizeof source,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&code,nullptr));
        ComPtr<ID3D11ComputeShader> shader;
        Require(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
        eam::ShaderSha256 nis{};
        const char identity[]="fc8f0300fec6f80250b8ec605265ba91c80f1d897fe799004f2cb95e14ca3d02";
        for(size_t i=0;i<nis.size();++i){unsigned v=0;sscanf_s(identity+i*2,"%2x",&v);nis[i]=static_cast<uint8_t>(v);}
        auto tag=[&](const eam::ShaderSha256* value){Require(shader->SetPrivateData(eam::kShaderSha256,value?32:0,value?value->data():nullptr));};
        context->CSSetShader(shader.Get(),nullptr,0);
        Texture input(device.Get(),320,240,DXGI_FORMAT_R16G16B16A16_FLOAT);
        Texture transformed(device.Get(),320,240,DXGI_FORMAT_R16G16B16A16_FLOAT);
        Texture output(device.Get(),640,480,DXGI_FORMAT_R16G16B16A16_FLOAT);
        Texture luma(device.Get(),320,240,DXGI_FORMAT_R8_UNORM);
        Texture coefficients(device.Get(),2,64,DXGI_FORMAT_R32G32B32A32_FLOAT);
        Constants constants;constants.f[12]=320.f/384;constants.f[13]=240.f/288;
        ComPtr<ID3D11Buffer> cb,shortCb;
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof constants;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=&constants;
        Require(device->CreateBuffer(&bd,&initial,&cb));bd.ByteWidth=48;Require(device->CreateBuffer(&bd,nullptr,&shortCb));
        auto bind=[&](Texture& frame,Texture& out,ID3D11Buffer* buffer){
            ID3D11UnorderedAccessView* none=nullptr;context->CSSetUnorderedAccessViews(0,1,&none,nullptr);
            ID3D11ShaderResourceView* views[]={frame.srv.Get(),coefficients.srv.Get(),coefficients.srv.Get()};
            context->CSSetShaderResources(0,3,views);auto* uav=out.uav.Get();context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
            context->CSSetConstantBuffers(0,1,&buffer);
        };
        nr::NisPass pass;
        unsigned warnings=0;auto log=[&](const char*){++warnings;};
        bind(input,output,cb.Get());
        check("untagged shader cannot replace NIS",!nr::FindNisPass(context.Get(),20,20,1,pass,log));nr::ReleaseNisPass(pass);
        eam::ShaderSha256 other{};tag(&other);
        check("unrelated shader with NIS bindings cannot replace a whole frame",!nr::FindNisPass(context.Get(),20,20,1,pass,log));nr::ReleaseNisPass(pass);
        tag(&nis);
        check("verified whole-frame NIS is accepted",nr::FindNisPass(context.Get(),20,20,1,pass,log)&&pass.inW==320&&pass.outW==640);nr::ReleaseNisPass(pass);
        check("partial viewport begins async readback",!nr::FindNisPass(context.Get(),12,12,1,pass,log));
        const auto beforeWarnings=warnings;
        bind(input,luma,shortCb.Get());tag(&other);
        check("LSFG luma pass does not masquerade as refused NIS",!nr::FindNisPass(context.Get(),4,3,1,pass,log)&&!nr::NisLayoutRefused());
        check("unrelated pass does not log a viewport error",warnings==beforeWarnings);
        bind(input,output,cb.Get());tag(&nis);context->Flush();
        bool found=false;const auto deadline=GetTickCount64()+2000;
        while(!found&&GetTickCount64()<deadline){found=nr::FindNisPass(context.Get(),12,12,1,pass,log);if(!found)Sleep(1);}
        check("NIS readback survives intervening LSFG passes",found&&pass.inW==320&&pass.outW==384&&pass.outX==128&&pass.outY==96);nr::ReleaseNisPass(pass);
        for(unsigned i=0;i<8;++i){
            bind(input,luma,shortCb.Get());tag(&other);nr::FindNisPass(context.Get(),4,3,1,pass,log);
            bind(transformed,output,cb.Get());tag(&nis);
            check("cached viewport accepts the NR-transformed input after LSFG",nr::FindNisPass(context.Get(),12,12,1,pass,log)&&pass.in==transformed.texture.Get());nr::ReleaseNisPass(pass);
        }
        ComPtr<ID3D11ShaderResourceView> current;context->CSGetShaderResources(0,1,&current);
        check("recognition preserves the transformed SRV binding",current.Get()==transformed.srv.Get());
        nr::ResetNisPass();constants.outX=640;context->UpdateSubresource(cb.Get(),0,nullptr,&constants,0,0);
        nr::FindNisPass(context.Get(),12,12,1,pass,log);context->Flush();
        const auto badDeadline=GetTickCount64()+2000;
        while(!nr::NisLayoutRefused()&&GetTickCount64()<badDeadline){nr::FindNisPass(context.Get(),12,12,1,pass,log);Sleep(1);}
        check("out-of-bounds NIS viewport stays rejected",nr::NisLayoutRefused()&&!pass.in);
        tag(&other);check("unrelated pass clears only the refusal report",!nr::FindNisPass(context.Get(),4,3,1,pass,log)&&!nr::NisLayoutRefused());
        nr::ResetNisPass();context->ClearState();context->Flush();
    } catch(const std::exception& e){printf("FAIL %s\n",e.what());++failures;}
    printf("%u failures\n",failures);return failures?1:0;
}
