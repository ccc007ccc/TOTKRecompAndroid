// Preserve the FSHA option -> program -> BNSH stage relationships used by the
// game. Layout references: TotK-Native/tools/shader_library.py and
// KillzXGaming/ShaderLibrary (Switch/BfshaLoader.cs, Structs.cs). Names/locations
// do not prove vertex formats, render-target formats or arbitrary runtime links.
#pragma once
#include "shader_archive.hpp"
#include <array>
#include <limits>

namespace shader_catalog {
using namespace shader_archive;
inline std::string string(View b, uint64_t at) {
    require(at != 0, "missing catalog string");
    auto size=b.u16(at);auto v=b.sub(at+2,uint64_t(size)+1);
    require(v.p[size]==0 && !memchr(v.p,0,size),"invalid catalog string");
    return {(const char*)v.p,size};
}
inline std::vector<std::string> dictionary(View b,uint64_t at) {
    if(!at)return {};
    require(b.sub(at,4).magic("_DIC",4) || b.u32(at)==0,"invalid catalog dictionary");
    auto count=b.u32(at+4);require(count<=65535,"catalog dictionary too large");b.sub(at+8,(uint64_t(count)+1)*16);
    std::vector<std::string> names;std::set<std::string> seen;
    for(unsigned i=0;i<=count;++i) {
        auto p=at+8+uint64_t(i)*16;
        require(b.u16(p+4)<=count && b.u16(p+6)<=count,"invalid catalog dictionary edge");
        if(i){auto name=string(b,b.u64(p+8));require(!name.empty()&&seen.insert(name).second,"duplicate/empty catalog name");names.push_back(std::move(name));}
    }
    return names;
}
struct Option {
    std::string name; bool dynamic=false;
    std::vector<std::string> choices;
    uint32_t mask=0; unsigned word=0,shift=0;
    int default_choice=-1;
    unsigned choice(const std::vector<uint32_t>& key) const {
        require(word<key.size() && shift<32,"option key field out of range");
        unsigned index=(key[word]&mask)>>shift;
        require(index<choices.size(),"option key choice out of range");return index;
    }
};
struct Attribute { std::string name; unsigned index; int location; };
struct Program {
    std::array<int,6> stages{{-1,-1,-1,-1,-1,-1}}; // STAGE_* order, not file order
    std::vector<uint32_t> key;
    std::vector<unsigned> choices;
    uint32_t variation=0,attribute_mask=0;
    std::vector<Attribute> vertex_inputs; // BNSH reflection names/locations; not formats by themselves
};
struct Model {
    std::string source,archive,name;
    unsigned static_words=0,dynamic_words=0;
    std::vector<Option> options;
    std::vector<Attribute> attributes;
    std::vector<Program> programs;
};
struct Record { Shader shader; std::set<std::string> sources; };
struct NamedProgram {
    std::string source,archive,name;
    std::vector<Program> variations;
};
struct Catalog {
    std::vector<Record> records;
    std::vector<Model> models;
    std::vector<NamedProgram> named_programs; // SHARCB stage groups; option names are not inferred
    // Retain every distinct code/control input, even if no FSHA selector refers
    // to it. Unassociated entries are unresolved, never silently discarded.
    std::map<Bytes,unsigned> identities;
    unsigned intern(Shader s,const std::string& source) {
        auto key=input_identity(s);
        auto [it,inserted]=identities.emplace(std::move(key),records.size());
        if(inserted)records.push_back({std::move(s),{}});
        auto& r=records.at(it->second);r.sources.insert(source);return it->second;
    }
    void seal(){identities.clear();}
};
inline void read_sharcb(View b,Catalog& out,const std::string& source) {
    std::vector<unsigned> records,stages;
    sharcb(b,[&](Shader s){stages.push_back(s.stage);records.push_back(out.intern(std::move(s),source));});
    auto array=0x150ull+b.u32(20),at=array+b.u32(array);auto bytes=b.u32(at),count=b.u32(at+4);
    require(count && count<=records.size() && bytes>=8ull+count*16ull,"invalid SHARCB named programs");
    require(at<=b.u32(28) && bytes<=b.u32(28)-at,"SHARCB programs overlap code");b.sub(at,bytes);
    auto end=at+bytes,cursor=at+8;std::string archive=b.str(0x150,b.u32(20));
    struct Group {std::string name;unsigned mask,first;};std::vector<Group> groups;
    for(unsigned i=0;i<count;++i) {
        require(cursor<=end && end-cursor>=16,"truncated SHARCB program");auto size=b.u32(cursor),length=b.u32(cursor+4),mask=b.u32(cursor+8),first=b.u32(cursor+12);
        require(size>=16 && size<=end-cursor && length && length<=size-16 && mask && mask<16 && first<records.size(),"invalid SHARCB program fields");
        groups.push_back({b.str(cursor+16,length),mask,first});cursor+=size;
    }
    require(cursor==end && groups[0].first==0,"incomplete SHARCB program table");
    for(unsigned i=0;i<groups.size();++i) {
        const auto& g=groups[i];unsigned stop=i+1<groups.size()?groups[i+1].first:records.size();
        std::vector<unsigned> expected;for(unsigned bit=0;bit<4;++bit)if(g.mask&(1u<<bit))expected.push_back(bit==3?5:bit);
        require(g.first<stop && (stop-g.first)%expected.size()==0,"invalid SHARCB stage span");
        NamedProgram p;p.source=source;p.archive=archive;p.name=g.name;
        for(unsigned j=g.first;j<stop;) {
            Program v;v.variation=p.variations.size();
            for(auto stage:expected){require(stages[j]==stage,"SHARCB named program stage mismatch");v.stages[stage]=records[j++];}
            p.variations.push_back(std::move(v));
        }
        out.named_programs.push_back(std::move(p));
    }
}
inline std::vector<Program> read_bnsh(View b,Catalog& out,const std::string& source) {
    // Run the same production bounds/control checks as the flat inventory.
    bnsh(b,[](Shader){});
    auto count=b.u32(0x7c);auto start=b.u64(0x80);
    const unsigned stages[]={0,3,4,2,1,5};
    std::map<uint64_t,std::pair<unsigned,unsigned>> codes;
    std::vector<Program> programs;
    for(unsigned i=0;i<count;++i) {
        auto program=b.u64(start+uint64_t(i)*64+16);Program p;p.variation=i;
        for(unsigned j=0;j<6;++j) {
            auto at=b.u64(program+8+j*8);if(!at)continue;
            auto it=codes.find(at);
            if(it==codes.end()) {
                unsigned id=0;
                nvn(b.sub(b.u64(at+16),b.u32(at+24)),b.sub(b.u64(at+8),b.u32(at+28)),stages[j],
                    [&](Shader s){id=out.intern(std::move(s),source);});
                it=codes.emplace(at,std::make_pair(id,stages[j])).first;
            }
            require(it->second.second==stages[j],"BNSH code shared across different stages");
            p.stages[stages[j]]=it->second.first;
        }
        require(p.stages[0]>=0 || p.stages[1]>=0 || p.stages[5]>=0,"empty catalog program");
        if(p.stages[0]>=0 && source.rfind("Effect/",0)==0) {
            auto reflection=b.u64(program+120);
            if(reflection){auto vertex=b.u64(reflection);
                if(vertex){auto inputs=dictionary(b,b.u64(vertex));auto slots=b.u32(vertex+56),nslots=b.u32(vertex+76);
                    require(inputs.size()<=nslots && nslots<=65535,"invalid effect input reflection slots");
                    b.sub(slots,uint64_t(nslots)*4);
                    for(unsigned a=0;a<inputs.size();++a){int location=int32_t(b.u32(uint64_t(slots)+a*4));
                        require(location>=-1 && location<32,"invalid effect input location");p.vertex_inputs.push_back({inputs[a],a,location});}
                }
            }
        }
        programs.push_back(std::move(p));
    }
    return programs;
}
inline void read_bfsha(View b,Catalog& out,const std::string& source) {
    require(b.magic("FSHA    ",8) && b.u32(8)==0x00090000 && b.u16(12)==0xfeff &&
            b.u16(20)==0 && b.u32(28)==b.n,"unsupported FSHA catalog");
    auto names=dictionary(b,b.u64(0x50));auto base=b.u64(0x48);
    require(!names.empty() && names.size()==b.u16(0x78) && base,"FSHA model count mismatch");b.sub(base,names.size()*256ull);
    for(unsigned i=0;i<names.size();++i) {
        auto at=base+uint64_t(i)*256;Model m;m.source=source;m.archive=string(b,b.u64(0x38));m.name=names[i];
        require(string(b,b.u64(at))==m.name,"FSHA model name mismatch");
        m.static_words=b.sub(at+234,1).p[0];m.dynamic_words=b.sub(at+235,1).p[0];
        for(unsigned dynamic=0;dynamic<2;++dynamic) {
            auto list=b.u64(at+(dynamic?24:8));auto labels=dictionary(b,b.u64(at+(dynamic?32:16)));
            require(labels.size()==b.u16(at+(dynamic?230:228)),"FSHA option count mismatch");
            if(!labels.empty())require(list,"missing FSHA options");b.sub(list,labels.size()*40ull);
            for(unsigned j=0;j<labels.size();++j) {
                auto pos=list+uint64_t(j)*40;Option o;o.name=labels[j];o.dynamic=dynamic;
                require(string(b,b.u64(pos))==o.name,"FSHA option name mismatch");o.choices=dictionary(b,b.u64(pos+8));
                o.default_choice=int16_t(b.u16(pos+26));
                require(o.choices.size()==b.u16(pos+24) && !o.choices.empty() && o.default_choice>=-1 &&
                        o.default_choice<int(o.choices.size()),"invalid FSHA option choices");
                int word=b.sub(pos+36,1).p[0];if(dynamic)word+=int(m.static_words)-b.sub(pos+31,1).p[0];
                o.word=word;o.mask=b.u32(pos+32);o.shift=b.sub(pos+37,1).p[0];
                require(word>=0 && o.word<m.static_words+m.dynamic_words && o.shift<32,"FSHA option key bounds");
                m.options.push_back(std::move(o));
            }
        }
        auto attrs=dictionary(b,b.u64(at+48));auto list=b.u64(at+40);
        require(attrs.size()==b.sub(at+236,1).p[0],"FSHA attribute count mismatch");b.sub(list,attrs.size()*2ull);
        for(unsigned j=0;j<attrs.size();++j) {
            auto a=b.sub(list+j*2,2);m.attributes.push_back({attrs[j],a.p[0],int8_t(a.p[1])});
        }
        auto file=b.u64(at+168);require(file,"missing catalog BNSH");
        auto variations=read_bnsh(b.sub(file,b.u32(file+28)),out,source+":"+m.name);
        auto programs=b.u64(at+136),keys=b.u64(at+144);auto count=b.u16(at+232);unsigned words=m.static_words+m.dynamic_words;
        require(!count || (programs && (!words || keys)),"missing FSHA program/key table");
        b.sub(programs,uint64_t(count)*64);b.sub(keys,uint64_t(count)*words*4);
        for(unsigned j=0;j<count;++j) {
            auto pos=programs+uint64_t(j)*64,ptr=b.u64(pos+32);require(ptr>=file+0xc0,"invalid FSHA variation pointer");
            auto rel=ptr-file-0xc0;require(!(rel%64) && rel/64<variations.size(),"FSHA variation out of range");
            auto p=variations[rel/64];p.attribute_mask=b.u32(pos+48);
            for(unsigned k=0;k<words;++k)p.key.push_back(b.u32(keys+(uint64_t(j)*words+k)*4));
            for(auto& o:m.options)p.choices.push_back(o.choice(p.key));
            m.programs.push_back(std::move(p));
        }
        out.models.push_back(std::move(m));
    }
}
inline Catalog read(Romfs& rom,const std::function<void(const std::string&)>& progress) {
    Catalog out;Decoder decoder;decoder.init(rom.read("Pack/ZsDic.pack.zs"));
    for(const auto& name:rom.archives()) {
        progress(name);auto bytes=rom.read(name);
        visit_archive(View(bytes),name,decoder,[&](ArchiveKind kind,const std::string& source,View b){
            if(kind==ArchiveKind::Bfsha)read_bfsha(b,out,source);
            else if(kind==ArchiveKind::Sharcb)read_sharcb(b,out,source);
            else {
                NamedProgram program;program.source=source;program.archive=source;program.name=source;
                program.variations=read_bnsh(b,out,source);out.named_programs.push_back(std::move(program));
            }
        });
    }
    out.seal();return out;
}
inline std::vector<std::set<unsigned>> vertex_partners(const Catalog& catalog) {
    std::vector<std::set<unsigned>> out(catalog.records.size());
    auto add=[&](const Program& p){if(p.stages[0]>=0 && p.stages[1]>=0) {
        require(catalog.records.at(p.stages[0]).shader.stage==0 && catalog.records.at(p.stages[1]).shader.stage==1,
                "catalog program stage mismatch");
        out[p.stages[1]].insert(p.stages[0]);
    }};
    for(const auto& m:catalog.models)for(const auto& p:m.programs)add(p);
    for(const auto& n:catalog.named_programs)for(const auto& p:n.variations)add(p);
    return out;
}
} // namespace shader_catalog
