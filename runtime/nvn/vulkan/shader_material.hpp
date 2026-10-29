// Read TotK BFRES v10 material/vertex metadata without decoding mesh geometry.
// Field layouts checked against KillzXGaming/BfresLibrary's Switch v10 readers.
// MCPK starts with a magicless Zstandard frame containing the FRES metadata;
// its separate FMSH geometry payload is deliberately not needed here.
#pragma once
#include "shader_catalog.hpp"

namespace shader_material {
using namespace shader_archive;
using Strings=std::map<uint64_t,std::string>;
inline Bytes metadata(const Bytes& input) {
    View b(input);
    require(b.magic("MCPK",4) && b.n>=13 && b.u16(6)==0 && b.sub(5,1).p[0]<=1,"unsupported MCPK metadata");
    uint32_t flags=b.u32(8);uint64_t capacity=uint64_t(flags>>5)<<(flags&15);
    require(capacity>=0xf0 && capacity<=256u<<20,"MCPK output exceeds metadata limit");
    Bytes frame{0x28,0xb5,0x2f,0xfd};frame.insert(frame.end(),input.begin()+12,input.end());
    size_t length=ZSTD_findFrameCompressedSize(frame.data(),frame.size());
    require(!ZSTD_isError(length),"truncated MCPK Zstandard frame");
    Bytes out(capacity);auto n=ZSTD_decompress(out.data(),out.size(),frame.data(),length);
    require(!ZSTD_isError(n) && n>=0xf0,"MCPK metadata decompression failed");out.resize(n);
    View r(out);require(r.magic("FRES    ",8) && r.u32(28)==out.size(),"MCPK metadata size mismatch");
    return out;
}
struct Reader {
    View b;const Strings& external;
    Reader(View data,const Strings& strings):b(data),external(strings) {
        require(b.magic("FRES    ",8) && b.u32(8)==0x000a0000 && b.u16(12)==0xfeff &&
                b.u16(20)==0 && b.u32(28)==b.n,"unsupported BFRES metadata");b.sub(0,0xf0);
    }
    std::string string(uint64_t id) const {
        if(!id)return {};
        auto it=external.find(id);if(it!=external.end())return it->second;
        return shader_catalog::string(b,id);
    }
    std::vector<std::string> dictionary(uint64_t at) const {
        if(!at)return {};
        require(b.sub(at,4).magic("_DIC",4) || b.u32(at)==0,"invalid BFRES dictionary");
        auto n=b.u32(at+4);require(n<=65535,"BFRES dictionary too large");b.sub(at+8,(uint64_t(n)+1)*16);
        std::vector<std::string> names;std::set<std::string> seen;
        for(unsigned i=0;i<=n;++i) {
            auto p=at+8+uint64_t(i)*16;require(b.u16(p+4)<=n && b.u16(p+6)<=n,"invalid BFRES dictionary edge");
            if(i){auto s=string(b.u64(p+8));require(!s.empty() && seen.insert(s).second,"duplicate/empty BFRES name");names.push_back(std::move(s));}
        }
        return names;
    }
};
inline Strings external_strings(const Bytes& metadata) {
    const Strings empty;Reader r(View(metadata),empty);View b(metadata);
    require(b.sub(0xee,1).p[0]&4,"BFRES does not hold external strings");
    auto names=r.dictionary(b.u64(0xc0));auto ids=b.u64(0xb8);b.sub(ids,names.size()*8ull);Strings out;
    for(unsigned i=0;i<names.size();++i)require(out.emplace(b.u64(ids+uint64_t(i)*8),names[i]).second,"duplicate BFRES string ID");
    return out;
}
struct Attribute { std::string name; uint16_t format,offset,buffer; };
struct Material {
    std::string name,archive,shader_model;
    std::map<std::string,std::string> attributes,options;
    std::set<std::string> default_options;
};
struct Shape {
    std::string name;
    unsigned material=0;
    std::vector<Attribute> attributes;
};
struct Model {
    std::string name;
    std::vector<Material> materials;
    std::vector<Shape> shapes;
};
inline std::vector<int> indices(View b,uint64_t at,unsigned used,unsigned total,bool wide) {
    std::vector<int> out;
    if(at) b.sub(at,(uint64_t(used)+total)*(wide?2:1));
    else require(used==total,"missing BFRES assignment index table");
    for(unsigned i=0;i<total;++i) {
        int index=!at?int(i):wide?int16_t(b.u16(at+(uint64_t(used)+i)*2)):int8_t(b.sub(at+used+i,1).p[0]);
        require(index>=-1 && index<int(used),"BFRES assignment index out of range");out.push_back(index);
    }
    return out;
}
inline Material material(const Reader& r,uint64_t at,const std::string& name) {
    auto b=r.b;b.sub(at,0xb0);require(b.sub(at,4).magic("FMAT",4),"invalid BFRES material");
    Material m;m.name=r.string(b.u64(at+8));require(m.name==name,"BFRES material name mismatch");
    auto info=b.u64(at+16);require(info,"missing BFRES shader info");b.sub(info,80);
    auto assign=b.u64(info);require(assign,"missing BFRES shader assignment");b.sub(assign,88);
    m.archive=r.string(b.u64(assign));m.shader_model=r.string(b.u64(assign+8));
    auto attr_names=r.dictionary(b.u64(assign+48));auto option_names=r.dictionary(b.u64(assign+64));
    unsigned used_attrs=b.sub(info+68,1).p[0],booleans=b.u16(info+70),total=b.u16(info+72);
    require(booleans<=total,"invalid BFRES option counts");
    auto attr_indices=indices(b,b.u64(info+16),used_attrs,attr_names.size(),false);
    auto attrs=b.u64(info+8);if(used_attrs)require(attrs,"missing BFRES attribute values");b.sub(attrs,used_attrs*8ull);
    for(unsigned i=0;i<attr_names.size();++i)if(attr_indices[i]>=0)
        m.attributes.emplace(attr_names[i],r.string(b.u64(attrs+uint64_t(attr_indices[i])*8)));
    auto option_indices=indices(b,b.u64(info+56),total,option_names.size(),true);
    auto bools=b.u64(info+40),values=b.u64(info+48);
    if(booleans){require(bools,"missing BFRES boolean values");b.sub(bools,((booleans+63ull)/64)*8);}
    if(total>booleans){require(values,"missing BFRES option values");b.sub(values,(total-booleans)*8ull);}
    for(unsigned i=0;i<option_names.size();++i) {
        int index=option_indices[i];
        if(index<0){m.default_options.insert(option_names[i]);continue;}
        // Boolean assignments are bits, while FSHA encodes their choices as 0/1.
        std::string value=unsigned(index)<booleans?((b.u64(bools+uint64_t(index/64)*8)>>(index%64))&1?"1":"0"):
            r.string(b.u64(values+uint64_t(index-booleans)*8));
        m.options.emplace(option_names[i],std::move(value));
    }
    return m;
}
inline std::vector<Attribute> vertex_attributes(const Reader& r,uint64_t at) {
    auto b=r.b;b.sub(at,0x58);require(b.sub(at,4).magic("FVTX",4),"invalid BFRES vertex buffer");
    auto names=r.dictionary(b.u64(at+16));auto list=b.u64(at+8);unsigned buffers=b.sub(at+77,1).p[0];
    require(names.size()==b.sub(at+76,1).p[0],"BFRES vertex attribute count mismatch");b.sub(list,names.size()*16ull);
    std::vector<Attribute> out;
    for(unsigned i=0;i<names.size();++i) {
        auto pos=list+uint64_t(i)*16;auto fmt=b.sub(pos+8,2);auto name=r.string(b.u64(pos));
        require(name==names[i] && b.u16(pos+14)<buffers,"invalid BFRES vertex attribute");
        out.push_back({name,uint16_t(uint16_t(fmt.p[0])<<8|fmt.p[1]),b.u16(pos+12),b.u16(pos+14)});
    }
    return out;
}
inline std::vector<Model> read(const Bytes& bytes,const Strings& strings) {
    Reader r(View(bytes),strings);auto b=r.b;
    auto names=r.dictionary(b.u64(0x30));auto base=b.u64(0x28);
    require(names.size()==b.u16(0xdc),"BFRES model count mismatch");b.sub(base,names.size()*0x78ull);
    std::vector<Model> out;
    for(unsigned i=0;i<names.size();++i) {
        auto at=base+uint64_t(i)*0x78;require(b.sub(at,4).magic("FMDL",4),"invalid BFRES model");
        Model m;m.name=r.string(b.u64(at+8));require(m.name==names[i],"BFRES model name mismatch");
        auto materials=r.dictionary(b.u64(at+0x40));auto material_list=b.u64(at+0x38);
        require(materials.size()==b.u16(at+0x6c),"BFRES material count mismatch");b.sub(material_list,materials.size()*0xb0ull);
        for(unsigned j=0;j<materials.size();++j)m.materials.push_back(material(r,material_list+uint64_t(j)*0xb0,materials[j]));
        auto shapes=r.dictionary(b.u64(at+0x30));auto shape_list=b.u64(at+0x28);
        require(shapes.size()==b.u16(at+0x6a),"BFRES shape count mismatch");b.sub(shape_list,shapes.size()*0x60ull);
        for(unsigned j=0;j<shapes.size();++j) {
            auto pos=shape_list+uint64_t(j)*0x60;require(b.sub(pos,4).magic("FSHP",4),"invalid BFRES shape");
            Shape s;s.name=r.string(b.u64(pos+8));s.material=b.u16(pos+0x52);
            require(s.name==shapes[j] && s.material<m.materials.size(),"BFRES shape material out of range");
            s.attributes=vertex_attributes(r,b.u64(pos+16));m.shapes.push_back(std::move(s));
        }
        out.push_back(std::move(m));
    }
    return out;
}
struct Match {
    int model=-1;
    std::vector<unsigned> programs;
    std::vector<std::string> unresolved;
};
// Material assignments constrain candidates; absent assignments and dynamic
// shader options remain unconstrained. A match is asset evidence, not proof
// that game code never changes an option or constructs additional geometry.
inline Match match(const Material& material,const shader_catalog::Catalog& catalog) {
    Match out;
    for(unsigned i=0;i<catalog.models.size();++i) {
        const auto& m=catalog.models[i];if(m.archive==material.archive && m.name==material.shader_model) {
            require(out.model<0,"ambiguous material shader model");out.model=i;
        }
    }
    if(out.model<0){out.unresolved.push_back("shader archive/model not found");return out;}
    const auto& model=catalog.models[out.model];std::vector<std::pair<unsigned,unsigned>> constraints;
    for(unsigned i=0;i<model.options.size();++i) {
        const auto& o=model.options[i];if(o.dynamic)continue;
        auto value=material.options.find(o.name);int choice=-1;
        if(value!=material.options.end()) {
            auto it=std::find(o.choices.begin(),o.choices.end(),value->second);
            if(it==o.choices.end()){out.unresolved.push_back("unknown option choice: "+o.name+"="+value->second);continue;}
            choice=it-o.choices.begin();
        } else if(material.default_options.count(o.name))choice=o.default_choice;
        if(choice>=0)constraints.emplace_back(i,choice);
    }
    for(unsigned i=0;i<model.programs.size();++i) {
        bool ok=true;for(auto [option,choice]:constraints)if(model.programs[i].choices[option]!=choice){ok=false;break;}
        if(ok)out.programs.push_back(i);
    }
    if(out.programs.empty())out.unresolved.push_back("no program matches material static options");
    return out;
}
// Repeated material lookup during preparation: start with the least common
// specified option instead of rescanning every program for each material.
class Matcher {
    const shader_catalog::Catalog& catalog;
    struct Index {
        std::map<std::string,unsigned> options;
        std::vector<std::vector<std::vector<unsigned>>> programs;
    };
    std::vector<Index> indexes;
    std::map<std::pair<std::string,std::string>,unsigned> models;
public:
    explicit Matcher(const shader_catalog::Catalog& c):catalog(c),indexes(c.models.size()) {
        for(unsigned mi=0;mi<c.models.size();++mi) {
            const auto& m=c.models[mi];auto& index=indexes[mi];
            require(models.emplace(std::make_pair(m.archive,m.name),mi).second,"ambiguous material shader model");
            index.programs.resize(m.options.size());
            for(unsigned o=0;o<m.options.size();++o)if(!m.options[o].dynamic) {
                index.options.emplace(m.options[o].name,o);index.programs[o].resize(m.options[o].choices.size());
                for(unsigned p=0;p<m.programs.size();++p)index.programs[o].at(m.programs[p].choices.at(o)).push_back(p);
            }
        }
    }
    Match find(const Material& material) const {
        Match out;auto found=models.find({material.archive,material.shader_model});
        if(found==models.end()){out.unresolved.push_back("shader archive/model not found");return out;}
        out.model=found->second;const auto& m=catalog.models[out.model];const auto& index=indexes[out.model];
        std::vector<std::pair<unsigned,unsigned>> constraints;const std::vector<unsigned>* candidates=nullptr;
        auto add=[&](unsigned option,int choice){if(choice<0)return;constraints.emplace_back(option,choice);
            const auto& ids=index.programs[option].at(choice);if(!candidates || ids.size()<candidates->size())candidates=&ids;};
        for(const auto& [name,value]:material.options) {
            auto f=index.options.find(name);if(f==index.options.end())continue;auto o=f->second;
            const auto& choices=m.options[o].choices;auto c=std::find(choices.begin(),choices.end(),value);
            if(c==choices.end())out.unresolved.push_back("unknown option choice: "+name+"="+value);
            else add(o,c-choices.begin());
        }
        for(const auto& name:material.default_options)if(!material.options.count(name)) {
            auto f=index.options.find(name);if(f!=index.options.end())add(f->second,m.options[f->second].default_choice);
        }
        auto accept=[&](unsigned p){for(auto [o,c]:constraints)if(m.programs[p].choices[o]!=c)return;out.programs.push_back(p);};
        if(candidates){for(auto p:*candidates)accept(p);}else for(unsigned p=0;p<m.programs.size();++p)accept(p);
        if(out.programs.empty())out.unresolved.push_back("no program matches material static options");return out;
    }
};
struct Input {
    std::string shader_name,vertex_name;
    int location;
    int format; // -1: absent from this shape; never assume float
};
inline std::vector<Input> inputs(const Shape& shape,const Material& material,const shader_catalog::Model& shader) {
    std::vector<Input> out;
    for(const auto& a:shader.attributes) {
        auto assign=material.attributes.find(a.name);int format=-1;std::string name;
        if(assign!=material.attributes.end()) {
            name=assign->second;for(const auto& v:shape.attributes)if(v.name==name){format=v.format;break;}
        }
        out.push_back({a.name,name,a.location,format});
    }
    return out;
}
} // namespace shader_material
