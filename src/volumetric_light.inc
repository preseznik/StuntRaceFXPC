    ComPtr<ID3D11VertexShader> volume_caster;
    ComPtr<ID3D11PixelShader> volume_mask,volume_integrate,volume_composite;
    ComPtr<ID3D11Buffer> volume_constants;
    ComPtr<ID3D11DepthStencilView> light_dsv;
    ComPtr<ID3D11ShaderResourceView> light_srv,scatter_srv;
    ComPtr<ID3D11RenderTargetView> scatter_rtv;
    UINT scatter_width{},scatter_height{};

    void init_volume() {
        static constexpr char source[]=
#include "volumetric_light.hlsl.inc"
        ;
        const D3D_SHADER_MACRO macros[]={{"MULTISAMPLE",coverage_samples>1?"1":"0"},{nullptr,nullptr}};
        const auto compile=[&](const char* entry,const char* profile){
            ComPtr<ID3DBlob> code,errors;
            if(FAILED(D3DCompile(source,sizeof(source),nullptr,macros,nullptr,entry,profile,0,0,&code,&errors)))
                throw std::runtime_error(errors?static_cast<const char*>(errors->GetBufferPointer()):entry);
            return code;
        };
        const auto vs_code=compile("caster_vertex","vs_5_0");
        check(device->CreateVertexShader(vs_code->GetBufferPointer(),vs_code->GetBufferSize(),nullptr,&volume_caster),"Volume caster vertex shader");
        const auto pixel=[&](const char* entry,auto& shader){
            const auto code=compile(entry,"ps_5_0");
            check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader),entry);
        };
        pixel("caster_pixel",volume_mask);pixel("integrate",volume_integrate);pixel("composite",volume_composite);
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=8*sizeof(float);cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&cb,nullptr,&volume_constants),"Volume parameters");
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=512;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R32_TYPELESS;d.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> tex;
        check(device->CreateTexture2D(&d,nullptr,&tex),"Volume light depth");
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D32_FLOAT;dv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        check(device->CreateDepthStencilView(tex.Get(),&dv,&light_dsv),"Volume shadow target");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R32_FLOAT;sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;
        check(device->CreateShaderResourceView(tex.Get(),&sv,&light_srv),"Volume shadow texture");
    }
    void draw_volume(const SceneTarget& target,const D3D11_VIEWPORT& viewport,float density) {
        const UINT w=UINT(std::ceil(viewport.Width*.5f)),h=UINT(std::ceil(viewport.Height*.5f));
        if(w!=scatter_width || h!=scatter_height) {
            D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.Format=DXGI_FORMAT_R16G16_FLOAT;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> tex;
            check(device->CreateTexture2D(&d,nullptr,&tex),"Half-resolution scattering");
            check(device->CreateRenderTargetView(tex.Get(),nullptr,scatter_rtv.ReleaseAndGetAddressOf()),"Scattering target");
            check(device->CreateShaderResourceView(tex.Get(),nullptr,scatter_srv.ReleaseAndGetAddressOf()),"Scattering texture");
            scatter_width=w;scatter_height=h;
        }
        const float params[]={viewport.TopLeftX,viewport.TopLeftY,viewport.Width,viewport.Height,
                              float(w),float(h),density,(forest_style || mesa_style)?1.f:0.f};
        context->UpdateSubresource(volume_constants.Get(),0,nullptr,params,0,0);
        context->VSSetConstantBuffers(3,1,volume_constants.GetAddressOf());
        context->PSSetConstantBuffers(3,1,volume_constants.GetAddressOf());
        context->OMSetRenderTargets(0,nullptr,light_dsv.Get());
        context->ClearDepthStencilView(light_dsv.Get(),D3D11_CLEAR_DEPTH,0,0);
        D3D11_VIEWPORT light_view{0,0,512,512,0,1},half{0,0,float(w),float(h),0,1};
        context->RSSetViewports(1,&light_view);context->RSSetState(sprite_solid.Get());
        context->OMSetDepthStencilState(reverse_depth.Get(),0);
        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const UINT stride=sizeof(GpuVertex),offset=0;
        context->IASetVertexBuffers(0,1,vertices.GetAddressOf(),&stride,&offset);
        context->VSSetShader(volume_caster.Get(),nullptr,0);context->PSSetShader(volume_mask.Get(),nullptr,0);
        const UINT instances=(forest_style || mesa_style)?1:2;
        // Include the foliage cutouts up to the start of the rival geometry.
        context->DrawInstanced(rival_start-cloud_vertices,instances,cloud_vertices,0);
        if(landscape_buffer) {
            context->IASetVertexBuffers(0,1,landscape_buffer.GetAddressOf(),&stride,&offset);
            context->DrawInstanced(landscape_vertex_count,instances,0,0);
        }
        context->OMSetRenderTargets(1,scatter_rtv.GetAddressOf(),nullptr);
        context->RSSetViewports(1,&half);context->IASetInputLayout(nullptr);
        context->VSSetShader(backdrop_vs.Get(),nullptr,0);context->PSSetShader(volume_integrate.Get(),nullptr,0);
        ID3D11ShaderResourceView* depths[]{light_srv.Get(),target.depth_srv.Get()};
        context->PSSetShaderResources(16,2,depths);context->Draw(3,0);
        context->OMSetRenderTargets(1,target.rtv.GetAddressOf(),nullptr);
        context->RSSetViewports(1,&viewport);context->PSSetShader(volume_composite.Get(),nullptr,0);
        context->PSSetShaderResources(18,1,scatter_srv.GetAddressOf());
        composite_overlay();context->Draw(3,0);
        ID3D11ShaderResourceView* empty[3]{};context->PSSetShaderResources(16,3,empty);
        context->OMSetBlendState(nullptr,nullptr,UINT_MAX);
    }
