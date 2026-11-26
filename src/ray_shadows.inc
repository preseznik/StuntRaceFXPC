// Immutable course geometry in a depth-first BVH. A miss skips a whole
// subtree, so the pixel shader never needs a traversal stack.
namespace ray_shadows {
using Point = std::array<float,3>;
struct Triangle {
    Point a, ab, ac;
    std::array<float,2> uv, uv_ab, uv_ac;
    float texture;
    Point color;
};
struct Node {
    Point low;
    UINT end;
    Point high;
    UINT first, count;
};
static_assert(sizeof(Node)==36 && sizeof(Triangle)==76);

struct Tree {
    std::vector<Node> nodes;
    std::vector<Triangle> faces;

    void subdivide(UINT first,UINT count,bool spatial=true) {
        Node node{{FLT_MAX,FLT_MAX,FLT_MAX},0,{-FLT_MAX,-FLT_MAX,-FLT_MAX},first,count};
        // Dynamic meshes are already grouped by actor and body part. Bound their
        // leaves once and merge child bounds instead of sorting every frame.
        if(spatial || count<=6) {
            for(UINT i=first;i<first+count;++i) {
                const auto& t=faces[i];
                for(unsigned axis=0;axis<3;++axis) {
                    float a=t.a[axis],b=a+t.ab[axis],c=a+t.ac[axis];
                    node.low[axis]=std::min({node.low[axis],a,b,c});
                    node.high[axis]=std::max({node.high[axis],a,b,c});
                }
            }
        }
        const auto index=nodes.size();
        nodes.push_back(node);
        if(count>6) {
            if(spatial) {
                unsigned axis=0;
                for(unsigned k=1;k<3;++k)
                    if(node.high[k]-node.low[k]>node.high[axis]-node.low[axis])axis=k;
                const auto center=[axis](const Triangle& t){return 3*t.a[axis]+t.ab[axis]+t.ac[axis];};
                auto begin=faces.begin()+first;
                std::nth_element(begin,begin+count/2,begin+count,
                    [&](const Triangle& a,const Triangle& b){return center(a)<center(b);});
            }
            nodes[index].count=0;
            const auto left=nodes.size();
            subdivide(first,count/2,spatial);
            const auto right=nodes.size();
            subdivide(first+count/2,count-count/2,spatial);
            if(!spatial)for(unsigned axis=0;axis<3;++axis) {
                nodes[index].low[axis]=std::min(nodes[left].low[axis],nodes[right].low[axis]);
                nodes[index].high[axis]=std::max(nodes[left].high[axis],nodes[right].high[axis]);
            }
        }
        nodes[index].end=static_cast<UINT>(nodes.size());
    }

    void add(std::span<const GpuVertex> vertices) {
        for(std::size_t i=0;i+2<vertices.size();i+=3) {
            const auto &a=vertices[i], &b=vertices[i+1], &c=vertices[i+2];
            // Detailed asphalt stores its three lateral offsets here so a secondary
            // ray can interpolate the same wheel paths as rasterization.
            Point color{(a.r+b.r+c.r)/3,(a.g+b.g+c.g)/3,(a.b+b.b+c.b)/3};
            if(a.texture==-26)color={a.r,b.r,c.r};
            faces.push_back({{a.x,a.y,a.z},{b.x-a.x,b.y-a.y,b.z-a.z},{c.x-a.x,c.y-a.y,c.z-a.z},
                             {a.u,a.v},{b.u-a.u,b.v-a.v},{c.u-a.u,c.v-a.v},a.texture,color});
        }
    }

    Tree() = default;
    void update_actors(std::span<const GpuVertex> vertices) {
        nodes.clear();
        faces.clear();
        add(vertices);
        if(!faces.empty())subdivide(0,static_cast<UINT>(faces.size()),false);
    }

    Tree(std::span<const GpuVertex> course,std::span<const GpuVertex> landscape) {
        faces.reserve((course.size()+landscape.size())/3);
        add(course);
        add(landscape);
        if(!faces.empty())subdivide(0,static_cast<UINT>(faces.size()));
    }
};

template<class T>
ComPtr<ID3D11ShaderResourceView> upload(ID3D11Device* device,const std::vector<T>& data) {
    ComPtr<ID3D11ShaderResourceView> view;
    if(data.empty())return view;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth=static_cast<UINT>(data.size()*sizeof(T));
    desc.StructureByteStride=sizeof(T);
    desc.Usage=D3D11_USAGE_IMMUTABLE;
    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA initial{data.data(),0,0};
    ComPtr<ID3D11Buffer> buffer;
    check(device->CreateBuffer(&desc,&initial,&buffer),"Course ray tracing buffer");
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;
    srv.Buffer.NumElements=static_cast<UINT>(data.size());
    check(device->CreateShaderResourceView(buffer.Get(),&srv,&view),"Course ray tracing view");
    return view;
}
// Reuse actor SRVs as wheels and suspension move. The root's end index
// bounds traversal when the current frame uses less than the allocated space.
template<class T>
struct ActorBuffer {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> view;
    UINT capacity{};
    bool active{};

    void update(ID3D11Device* device,ID3D11DeviceContext* context,const std::vector<T>& values) {
        active=!values.empty();
        if(!active)return;
        const UINT size=static_cast<UINT>(values.size());
        if(size>capacity) {
            capacity=std::max(size,2*capacity);
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth=capacity*sizeof(T);
            desc.StructureByteStride=sizeof(T);
            desc.Usage=D3D11_USAGE_DEFAULT;
            desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            check(device->CreateBuffer(&desc,nullptr,buffer.ReleaseAndGetAddressOf()),"Reflection actor buffer");
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;
            srv.Buffer.NumElements=capacity;
            check(device->CreateShaderResourceView(buffer.Get(),&srv,view.ReleaseAndGetAddressOf()),"Reflection actor view");
        }
        D3D11_BOX used{0,0,0,static_cast<UINT>(size*sizeof(T)),1,1};
        context->UpdateSubresource(buffer.Get(),0,&used,values.data(),0,0);
    }
    ID3D11ShaderResourceView* current() const {return active?view.Get():nullptr;}
};
}
