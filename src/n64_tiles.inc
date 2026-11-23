namespace n64 {
// Two seamless material tiles and a small sky reflection share the RGB channels.
// Build the 32x32 base once, then box-filter each smaller mip before uploading.
ComPtr<ID3D11ShaderResourceView> material_tiles(ID3D11Device* device) {
    constexpr int size=32,levels=6;
    std::array<std::vector<std::uint8_t>,levels> pixels;
    auto& base=pixels[0];base.resize(size*size*4);
    auto noise=[](int x,int y) {
        unsigned h=(unsigned(x)*0x9e3779b9u)^(unsigned(y)*0x85ebca6bu);
        h^=h>>16;h*=0x7feb352du;h^=h>>15;
        return float(h&255)/255.f;
    };
    for(int y=0;y<size;++y)for(int x=0;x<size;++x) {
        const int i=4*(y*size+x);
        const float fleck=noise(x,y),cluster=noise(x/4,y/4);
        base[i]=std::uint8_t(255*(.58f*fleck+.42f*cluster));
        base[i+1]=std::uint8_t(255*(.32f*fleck+.43f*noise(x/2,y/4)+.25f*cluster));
        const float horizon=.45f+.065f*std::cos(x*.19635f)+.035f*std::sin(x*.3927f);
        base[i+2]=std::uint8_t(y/31.f<horizon?58+18*cluster:162+60*y/31.f);
        base[i+3]=255;
    }
    std::array<D3D11_SUBRESOURCE_DATA,levels> data{};
    for(int level=0;level<levels;++level) {
        const int w=size>>level;
        if(level) {
            pixels[level].resize(w*w*4);
            const auto& src=pixels[level-1];
            for(int y=0;y<w;++y)for(int x=0;x<w;++x)for(int c=0;c<4;++c) {
                int i=(y*2*w*2+x*2)*4+c;
                pixels[level][(y*w+x)*4+c]=(src[i]+src[i+4]+src[i+8*w]+src[i+8*w+4]+2)/4;
            }
        }
        data[level]={pixels[level].data(),UINT(w*4),0};
    }
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=size;desc.MipLevels=levels;
    desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    check(device->CreateTexture2D(&desc,data.data(),&texture),"N64 material tiles");
    ComPtr<ID3D11ShaderResourceView> view;
    check(device->CreateShaderResourceView(texture.Get(),nullptr,&view),"N64 material tile view");
    return view;
}
}
