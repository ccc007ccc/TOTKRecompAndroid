#include "../runtime/nvn/vulkan/shader_material.hpp"
#include <cassert>
#include <iostream>
using namespace shader_archive;
struct Builder {
    Bytes b;
    explicit Builder(size_t n):b(n){}
    size_t alloc(size_t n){size_t at=(b.size()+7)&~size_t(7);b.resize(at+n);return at;}
    void put(size_t at,uint64_t n,unsigned width=4){for(unsigned i=0;i<width;++i)b.at(at+i)=n>>(i*8);}
    void magic(size_t at,const char* s,size_t n){memcpy(b.data()+at,s,n);}
    size_t string(const std::string& s){auto at=alloc(s.size()+3);put(at,s.size(),2);memcpy(b.data()+at+2,s.c_str(),s.size()+1);return at;}
    size_t dict(const std::vector<std::string>& names){auto at=alloc(8+(names.size()+1)*16);magic(at,"_DIC",4);put(at+4,names.size());
        for(unsigned i=0;i<names.size();++i){auto key=string(names[i]);put(at+8+(i+1)*16+8,key,8);}return at;}
    void header(const char* tag,uint32_t version){magic(0,tag,8);put(8,version);put(12,0xfeff,2);}
    Bytes finish(){put(28,b.size());return std::move(b);}
};
static Bytes bnsh_fixture() {
    Builder w(0x140);w.header("BNSH\0\0\0\0",0x00020201);w.magic(0x60,"grsc",4);w.put(0x7c,2);w.put(0x80,0xc0,8);
    size_t shader[3];
    for(unsigned i=0;i<3;++i){shader[i]=w.alloc(64);auto c=w.alloc(0x880),b=w.alloc(0xa0);unsigned stage=i?1:0;
        w.put(shader[i]+8,c,8);w.put(shader[i]+16,b,8);w.put(shader[i]+24,0xa0);w.put(shader[i]+28,0x880);
        w.put(c,0x98761234);w.put(c+0x20,0x880);w.put(c+0x700,0xa0);w.put(c+0x704,0xa0);w.put(c+0x708,0x30);w.put(c+0x714,stage);
        w.put(b,0x12345678);w.put(b+0x30,(stage?5:1)<<10);w.put(b+0x98,i);}
    auto reflections=w.alloc(48),vertex=w.alloc(96),slots=w.alloc(8);
    w.put(reflections,vertex,8);w.put(vertex,w.dict({"sysLocalPosAttr","sysPosAttr"}),8);
    w.put(vertex+56,slots);w.put(vertex+76,2);w.put(slots,0);w.put(slots+4,1);
    for(unsigned i=0;i<2;++i){auto p=w.alloc(144);w.put(0xc0+i*64+16,p,8);w.put(p+8,shader[0],8);w.put(p+40,shader[i+1],8);w.put(p+120,reflections,8);}
    return w.finish();
}
static Bytes fsha_fixture() {
    Builder w(0x80);w.header("FSHA    ",0x00090000);auto m=w.alloc(256);
    w.put(0x38,w.string("test_archive"),8);w.put(0x48,m,8);w.put(0x50,w.dict({"test_model"}),8);w.put(0x78,1,2);w.put(m,w.string("test_model"),8);
    w.put(m+228,1,2);w.put(m+230,1,2);w.put(m+232,2,2);w.put(m+234,1,1);w.put(m+235,1,1);
    for(unsigned dynamic=0;dynamic<2;++dynamic){auto list=w.alloc(40);std::string name=dynamic?"pass":"enabled";
        w.put(m+(dynamic?24:8),list,8);w.put(m+(dynamic?32:16),w.dict({name}),8);
        w.put(list,w.string(name),8);w.put(list+8,w.dict(dynamic?std::vector<std::string>{"color","depth"}:std::vector<std::string>{"0","1"}),8);
        w.put(list+24,2,2);w.put(list+26,0,2);w.put(list+31,dynamic?3:0,1);w.put(list+32,dynamic?4:1);w.put(list+36,dynamic?3:0,1);w.put(list+37,dynamic?2:0,1);}
    auto attrs=w.alloc(4);w.put(attrs,0,1);w.put(attrs+1,0,1);w.put(attrs+2,1,1);w.put(attrs+3,3,1);
    w.put(m+40,attrs,8);w.put(m+48,w.dict({"position","bones"}),8);w.put(m+236,2,1);
    auto binary=bnsh_fixture();auto at=w.alloc(binary.size());memcpy(w.b.data()+at,binary.data(),binary.size());w.put(m+168,at,8);
    auto programs=w.alloc(128),keys=w.alloc(16);w.put(m+136,programs,8);w.put(m+144,keys,8);
    w.put(keys+8,1);w.put(keys+12,4);
    for(unsigned i=0;i<2;++i){w.put(programs+i*64+32,at+0xc0+i*64,8);w.put(programs+i*64+48,3);}
    return w.finish();
}
static Bytes sharcb_fixture() {
    auto bn=bnsh_fixture();std::vector<Shader> shaders;bnsh(View(bn),[&](Shader s){shaders.push_back(std::move(s));});assert(shaders.size()==3);
    Builder w(0x150);w.put(0,0x53484142);w.put(4,9);w.put(12,5);w.put(20,8);auto name=w.alloc(8);w.magic(name,"fixture",8);
    auto array=w.alloc(8+4*(16+16+0x880));w.put(array,8+4*(16+16+0x880));w.put(array+4,4);
    unsigned ids[]={0,1,0,2};size_t payloads[4];
    for(unsigned i=0;i<4;++i){auto at=array+8+i*(16+16+0x880),payload=at+16;payloads[i]=payload;
        w.put(at,16+16+0x880);w.put(at+4,shaders[ids[i]].stage);w.put(at+12,16+0x880);
        w.put(payload+8,0x880);w.put(payload+12,16);memcpy(w.b.data()+payload+16,shaders[ids[i]].control.data(),0x880);}
    auto programs=w.alloc(40);w.put(programs,40);w.put(programs+4,1);w.put(programs+8,32);w.put(programs+12,5);w.put(programs+16,3);w.magic(programs+24,"copy",5);
    auto code=(w.b.size()+4095)&~size_t(4095);w.b.resize(code);w.put(28,code);w.put(24,4*0xa0);
    for(unsigned i=0;i<4;++i){w.put(payloads[i],i*0xa0,8);w.b.insert(w.b.end(),shaders[ids[i]].binary.begin(),shaders[ids[i]].binary.end());}
    w.put(8,w.b.size());return std::move(w.b);
}
static bool rejected(const std::function<void()>& f){try{f();return false;}catch(const std::runtime_error&){return true;}}
static Bytes sarc_fixture(const std::string& name,const Bytes& member) {
    Builder w(0x100);w.magic(0,"SARC",4);w.put(4,20,2);w.put(6,0xfeff,2);w.put(12,0x100);
    w.magic(20,"SFAT",4);w.put(24,12,2);w.put(26,1,2);w.put(36,0x01000000);w.put(44,member.size());
    w.magic(48,"SFNT",4);w.put(52,8,2);assert(name.size()+1<=0x100-56);w.magic(56,name.c_str(),name.size()+1);
    w.b.insert(w.b.end(),member.begin(),member.end());w.put(8,w.b.size());return std::move(w.b);
}
static Bytes effect_fixture() {
    auto shader=bnsh_fixture();Builder v(0x80);v.header("VFXB    ",0x00330400);v.put(0x16,0x40,2);
    v.magic(0x40,"GRSN",4);v.put(0x48,0x20);v.put(0x4c,UINT32_MAX);
    v.magic(0x60,"GRSR",4);v.put(0x64,shader.size());v.put(0x68,UINT32_MAX);v.put(0x74,0x20);
    auto data=v.alloc(shader.size());assert(data==0x80);memcpy(v.b.data()+data,shader.data(),shader.size());
    auto sibling=v.alloc(32);v.put(0x6c,sibling-0x60);v.magic(sibling,"GRSC",4);
    v.put(sibling+4,shader.size());v.put(sibling+8,UINT32_MAX);v.put(sibling+12,UINT32_MAX);v.put(sibling+20,0x20);
    data=v.alloc(shader.size());memcpy(v.b.data()+data,shader.data(),shader.size());auto blob=v.finish();
    Builder w(0x100);w.magic(0,"YB\5\0",4);w.put(4,0x10);w.put(12,0x40);
    w.put(0x10,0x1c2);w.put(0x14,12);w.put(0x18,20);w.magic(0x1c,"PtclBin",8);
    w.put(0x40,0x1c1);w.put(0x44,0xa2000000);w.put(0x48,0xf8);
    w.put(0xf8,blob.size());w.put(0xfc,0x100);w.b.insert(w.b.end(),blob.begin(),blob.end());return std::move(w.b);
}
int main(int argc,char** argv) {
    auto effect=effect_fixture();Decoder effect_decoder;shader_catalog::Catalog effects;unsigned embedded=0;
    visit_archive(View(effect),"Effect/test.esetb.byml",effect_decoder,[&](ArchiveKind kind,const std::string& source,View b){
        assert(kind==ArchiveKind::Bnsh && source.find(":PtclBin/GRSN-")!=std::string::npos);
        auto programs=shader_catalog::read_bnsh(b,effects,source);assert(programs.size()==2 && programs[0].vertex_inputs.size()==2);
        assert(programs[0].vertex_inputs[0].name=="sysLocalPosAttr" && programs[0].vertex_inputs[1].location==1);++embedded;
    });assert(embedded==2 && effects.records.size()==3);
    for(size_t n=0;n<effect.size();++n)assert(rejected([&]{effect_shaders(View(effect.data(),n),"truncated",[](auto,auto,auto){});}));
    for(auto [offset,value]:std::vector<std::pair<size_t,uint32_t>>{
        {4,UINT32_MAX},{12,UINT32_MAX},{0x14,UINT32_MAX},{0x48,UINT32_MAX},{0xfc,3},{0x148,0},
        {0x14c,0},{0x164,UINT32_MAX},{0x16c,0},{0x174,UINT32_MAX}}) {
        auto bad=effect;for(unsigned i=0;i<4;++i)bad[offset+i]=value>>(8*i);
        assert(rejected([&]{effect_shaders(View(bad),"bad",[](auto,auto,auto){});}));
    }
    // Identical instruction bytes need not mean identical native inputs: the
    // skipped compute prefix participates in upstream SHA-256 request keys.
    shader_catalog::Catalog prefix_catalog;
    Shader first{};first.stage=5;first.constants=8;first.binary={1,2,3};first.control={4,5};first.code_prefix.resize(0x100);
    auto second=first;second.code_prefix[0x80]=1;
    auto first_id=prefix_catalog.intern(first,"first"),second_id=prefix_catalog.intern(second,"second");
    assert(first_id!=second_id && prefix_catalog.records.size()==2);
    assert(prefix_catalog.intern(first,"duplicate")==first_id && prefix_catalog.records.size()==2);
    assert(prefix_catalog.records[first_id].sources.count("duplicate"));
    auto bytes=fsha_fixture();shader_catalog::Catalog c;shader_catalog::read_bfsha(View(bytes),c,"fixture");
    assert(c.records.size()==3 && c.models.size()==1);auto& m=c.models[0];assert(m.programs.size()==2);
    assert(m.programs[0].stages[0]==m.programs[1].stages[0] && m.programs[0].stages[1]!=m.programs[1].stages[1]);
    auto partners=shader_catalog::vertex_partners(c);assert(partners[m.programs[0].stages[1]]==std::set<unsigned>({unsigned(m.programs[0].stages[0])}));
    assert(m.options[1].word==1 && m.programs[1].choices==std::vector<unsigned>({1,1}));
    assert(m.attributes[1].location==3);
    shader_material::Material mat;mat.archive="test_archive";mat.shader_model="test_model";
    shader_material::Matcher matcher(c);
    auto match=shader_material::match(mat,c);assert(match.programs==std::vector<unsigned>({0,1})); // unspecified is not default
    assert(matcher.find(mat).programs==match.programs);
    mat.options["enabled"]="1";mat.options["pass"]="color";
    match=shader_material::match(mat,c);assert(match.unresolved.empty() && match.programs==std::vector<unsigned>({1})); // dynamic stays free
    assert(matcher.find(mat).programs==match.programs);
    mat.options.erase("enabled");mat.default_options.insert("enabled");
    match=shader_material::match(mat,c);assert(match.programs==std::vector<unsigned>({0}));
    assert(matcher.find(mat).programs==match.programs);
    mat.options["enabled"]="unknown";assert(!shader_material::match(mat,c).unresolved.empty());
    assert(matcher.find(mat).unresolved==shader_material::match(mat,c).unresolved);
    mat.attributes["position"]="_p0";mat.attributes["bones"]="_i0";
    shader_material::Shape shape;shape.attributes.push_back({"_p0",0x518,0,0});
    auto inputs=shader_material::inputs(shape,mat,m);assert(inputs[0].format==0x518 && inputs[1].format==-1);
    // Rank table has used indices first, followed by one signed index per key.
    Bytes index{0,2,1,255,0};assert(shader_material::indices(View(index),0,3,3,false)==std::vector<int>({0,1,2}));
    Bytes indexed{0,0,2,1,255,0};assert(shader_material::indices(View(indexed),1,2,3,false)==std::vector<int>({1,-1,0}));
    indexed[3]=2;assert(rejected([&]{shader_material::indices(View(indexed),1,2,3,false);}));
    // A dictionary edge, selector pointer, option bit position and key choice
    // must all be bounded. No malformed association may become a valid graph.
    auto bad=bytes;Builder w(0);w.b=bad;auto model=View(bad).u64(0x48);auto option=View(bad).u64(model+8);
    w.put(option+37,32,1);assert(rejected([&]{shader_catalog::Catalog x;shader_catalog::read_bfsha(View(w.b),x,"bad");}));
    w.b=bytes;w.put(option+32,3);auto keys=View(bytes).u64(model+144);w.put(keys,3);
    assert(rejected([&]{shader_catalog::Catalog x;shader_catalog::read_bfsha(View(w.b),x,"bad");}));
    w.b=bytes;auto programs=View(bytes).u64(model+136);w.put(programs+32,UINT64_MAX,8);
    assert(rejected([&]{shader_catalog::Catalog x;shader_catalog::read_bfsha(View(w.b),x,"bad");}));
    w.b=bytes;auto dict=View(bytes).u64(0x50);w.put(dict+12,2,2);
    assert(rejected([&]{shader_catalog::Catalog x;shader_catalog::read_bfsha(View(w.b),x,"bad");}));
    for(size_t n=0;n<bytes.size();n+=37)assert(rejected([&]{shader_catalog::Catalog x;shader_catalog::read_bfsha(View(bytes.data(),n),x,"truncated");}));
    assert(rejected([&]{shader_material::metadata(Bytes(12));}));
    // Header-only metadata is valid without GPU geometry; round-trip a
    // magicless Zstandard frame to exercise the native MCPK metadata reader.
    Builder empty(0xf0);empty.header("FRES    ",0x000a0000);auto fres=empty.finish();
    Bytes z(ZSTD_compressBound(fres.size()));auto size=ZSTD_compress(z.data(),z.size(),fres.data(),fres.size(),1);assert(!ZSTD_isError(size));z.resize(size);
    Builder mc(12);mc.magic(0,"MCPK",4);mc.put(4,0x101,2);mc.put(8,0xf0u<<5);mc.b.insert(mc.b.end(),z.begin()+4,z.end());
    assert(shader_material::metadata(mc.b)==fres);shader_material::Strings strings;assert(shader_material::read(fres,strings).empty());
    mc.b.pop_back();assert(rejected([&]{shader_material::metadata(mc.b);}));
    auto sc=sharcb_fixture();shader_catalog::Catalog named;shader_catalog::read_sharcb(View(sc),named,"named-fixture");
    assert(named.records.size()==3 && named.named_programs.size()==1 && named.named_programs[0].name=="copy" && named.named_programs[0].variations.size()==2);
    auto array=0x150+View(sc).u32(20);auto groups=array+View(sc).u32(array);Builder damaged(0);damaged.b=sc;damaged.put(groups+16,1);
    assert(rejected([&]{shader_catalog::Catalog x;shader_catalog::read_sharcb(View(damaged.b),x,"bad");}));
    // An archive outside Shader/ must expose its real members and paired stages.
    Decoder decoder;auto sdk_binary=bnsh_fixture();Builder sdk_version(0);sdk_version.b=sdk_binary;sdk_version.put(8,0x00020102);
    auto ui=sarc_fixture("bgsh/__ArchiveShader.bnsh",sdk_version.b);
    shader_catalog::Catalog ui_catalog;
    visit_archive(View(ui),"UI/LayoutArchive/Common.blarc",decoder,[&](ArchiveKind kind,const std::string& source,View v){
        assert(kind==ArchiveKind::Bnsh && source=="UI/LayoutArchive/Common.blarc:bgsh/__ArchiveShader.bnsh");
        auto programs=shader_catalog::read_bnsh(v,ui_catalog,source);
        assert(programs.size()==2 && programs[0].stages[0]==programs[1].stages[0]);
    });
    assert(ui_catalog.records.size()==3);
    auto malformed=ui;malformed.pop_back();assert(rejected([&]{visit_archive(View(malformed),"x.blarc",decoder,[](auto,auto,auto){});}));
    auto nested=ui;for(unsigned i=0;i<9;++i)nested=sarc_fixture("nested.sarc",nested);
    assert(rejected([&]{visit_archive(View(nested),"x.sarc",decoder,[](auto,auto,auto){});}));
    if(argc==2) {
        Romfs rom(argv[1]);auto real=shader_catalog::read(rom,[](auto){});assert(real.records.size()==28634 && real.models.size()==23);
        std::set<Bytes> flat;inventory(rom,[&](Shader s){flat.insert(input_identity(s));},[](auto){});
        assert(flat.size()==real.records.size());for(const auto& r:real.records)assert(flat.erase(input_identity(r.shader))==1);assert(flat.empty());
        std::set<int> associated;for(const auto& model:real.models)for(const auto& p:model.programs)for(int id:p.stages)if(id>=0)associated.insert(id);
        for(const auto& group:real.named_programs)for(const auto& p:group.variations)for(int id:p.stages)if(id>=0)associated.insert(id);
        assert(real.named_programs.size()>66 && associated.size()==real.records.size());
        bool agl=false,ui_found=false,sdk_found=false,effect_found=false;
        for(const auto& r:real.records)for(const auto& source:r.sources) {
            agl|=source.find("Lib/agl/")==0;ui_found|=source.find("UI/LayoutArchive/")==0;sdk_found|=source=="Lib/NintendoSDK/TextureShader.bnsh";
            effect_found|=source.find("Effect/")==0 && source.find(":PtclBin/")!=std::string::npos;
        }
        assert(agl && ui_found && sdk_found && effect_found);
        auto external=shader_material::external_strings(shader_material::metadata(rom.read("Shader/ExternalBinaryString.bfres.mc")));
        auto models=shader_material::read(shader_material::metadata(rom.read("Model/Animal_Bass.Bass.bfres.mc")),external);
        assert(models.size()==1 && models[0].materials.size()==2 && models[0].shapes.size()==2);
        for(const auto& material:models[0].materials){auto found=shader_material::match(material,real);assert(found.unresolved.empty() && !found.programs.empty());}
        std::cout<<"Real corpus: all 28634 code/control inputs preserved, including library/UI/SDK/effect programs; material selectors resolve\n";
    }
    std::cout<<"PASS: program links, option masks/defaults, dynamic options, vertex mappings and malformed-input rejection\n";
}
