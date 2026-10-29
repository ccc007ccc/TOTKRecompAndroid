// Preparation-only SPIR-V interface specialization. Maxwell registers are raw
// uint32 bits: changing an input/output numeric type requires bitcasts at the
// interface, not changes to the shader's arithmetic. Unknown pointer operations
// are rejected and stop preparation before a completion marker is written.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace shader_interface {
using Words=std::vector<uint32_t>;
inline void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Type {uint32_t op=0,a=0,b=0;};
struct Instruction {size_t at;uint32_t count,op;};
class Module {
    const uint32_t* source;
    size_t size;
    uint32_t bound,next;
    std::vector<Type> types;
    std::vector<uint32_t> result_type,root;
    std::vector<int> location,binding,set;
    std::vector<uint8_t> kind;
    std::vector<Instruction> instructions;
    Words new_types,new_constants;
    std::map<std::pair<uint32_t,unsigned>,uint32_t> numeric_types;
    std::map<std::pair<uint32_t,uint32_t>,uint32_t> pointers;
    std::map<uint32_t,uint32_t> zeros;
    uint32_t integer[2]{};
    uint32_t tint=0;
    uint32_t id(){require(next<0x3fffffu,"specialized SPIR-V ID limit");return next++;}
    void valid_id(uint32_t n)const{require(n && n<bound,"SPIR-V ID out of range");}
    bool pointer(uint32_t type)const{return type<types.size() && types[type].op==32;}
    uint32_t numeric(uint32_t type,unsigned k) {
        valid_id(type);require(k==1 || k==2,"invalid interface type");auto t=types[type];
        require((t.op==22 && t.a==32) || (t.op==23 && t.b>=2 && t.b<=4),"interface is not 32-bit float scalar/vector");
        auto found=numeric_types.find({type,k});if(found!=numeric_types.end())return found->second;
        uint32_t out;
        if(t.op==22) {
            auto& scalar=integer[k-1];if(!scalar){scalar=id();new_types.insert(new_types.end(),{4u<<16|21,scalar,32,k==1?1u:0u});}out=scalar;
        } else {
            auto scalar=numeric(t.a,k);out=0;
            for(uint32_t i=1;i<bound;++i)if(types[i].op==23 && types[i].a==scalar && types[i].b==t.b){out=i;break;}
            if(!out){out=id();new_types.insert(new_types.end(),{4u<<16|23,out,scalar,t.b});}
        }
        numeric_types[{type,k}]=out;return out;
    }
    uint32_t retyped_pointer(uint32_t type,unsigned k) {
        valid_id(type);require(pointer(type),"interface pointer type missing");auto t=types[type];auto base=numeric(t.b,k);
        auto found=pointers.find({t.a,base});if(found!=pointers.end())return found->second;
        auto out=id();new_types.insert(new_types.end(),{4u<<16|32,out,t.a,base});pointers[{t.a,base}]=out;return out;
    }
    uint32_t zero(uint32_t type) {
        valid_id(type);auto t=types[type];require(t.op==22 || (t.op==23 && types.at(t.a).op==22),"unexpected tint value type");
        auto found=zeros.find(type);if(found!=zeros.end())return found->second;
        auto out=id();new_constants.insert(new_constants.end(),{3u<<16|46,type,out});zeros[type]=out;return out;
    }
    void copy(Words& out,const Instruction& i)const{out.insert(out.end(),source+i.at,source+i.at+i.count);}
public:
    Module(const uint32_t* words,size_t n,int stage,const uint8_t* kinds):source(words),size(n) {
        require(words && kinds && n>=5 && n<=4u*1024*1024 && words[0]==0x07230203,"invalid SPIR-V module");
        require(stage==0 || stage==1,"interface specialization needs a graphics stage");
        bound=next=words[3];require(bound>0 && bound<=n,"unsupported sparse SPIR-V IDs");
        types.resize(bound);result_type.resize(bound);root.resize(bound);kind.resize(bound);
        location.assign(bound,-1);binding.assign(bound,-1);set.assign(bound,-1);
        for(size_t at=5;at<n;) {
            uint32_t count=words[at]>>16,op=words[at]&65535;require(count && count<=n-at,"truncated SPIR-V instruction");
            instructions.push_back({at,count,op});const auto* w=words+at;
            if(op>=19 && op<=39){require(count>=2,"invalid SPIR-V type");valid_id(w[1]);types[w[1]]={op,count>2?w[2]:0,count>3?w[3]:0};}
            if(op==21 && count==4 && w[2]==32 && w[3]<=1)integer[w[3]?0:1]=w[1];
            if(op==32 && count==4)pointers[{w[2],w[3]}]=w[1];
            if(op==59 || op==65 || op==66 || op==83 || op==61){require(count>=4,"invalid SPIR-V value");valid_id(w[1]);valid_id(w[2]);result_type[w[2]]=w[1];}
            if(op==65 || op==66 || op==83 || op==61)valid_id(w[3]);
            if(op==62){require(count>=3,"invalid SPIR-V store");valid_id(w[1]);valid_id(w[2]);}
            if(op==15){require(count>=4 && w[1]==(stage==0?0u:4u),"SPIR-V entry point stage mismatch");}
            if(op==71 && count>=4){valid_id(w[1]);if(w[2]==30)location[w[1]]=w[3];if(w[2]==33)binding[w[1]]=w[3];if(w[2]==34)set[w[1]]=w[3];}
            at+=count;
        }
        for(const auto& i:instructions)if(i.op==59) {
            const auto* w=source+i.at;auto variable=w[2];
            if((stage==0?w[3]==1:w[3]==3) && location[variable]>=0) {
                auto loc=unsigned(location[variable]);require(loc<(stage==0?32u:8u),"interface location out of range");
                require(kinds[loc]<=2,"invalid requested numeric type");
                if(kinds[loc]){require(i.count==4,"interface initializer is unsupported");root[variable]=variable;kind[variable]=kinds[loc];retyped_pointer(w[1],kinds[loc]);}
            }
            if(stage==1 && kinds[0] && w[3]==2 && binding[variable]==19 && set[variable]==1){tint=variable;root[variable]=variable;}
        }
        // Generated GLSL uses direct access chains and loads/stores. Track aliases
        // without accepting pointer merges or passing changed pointers to functions.
        for(const auto& i:instructions) {
            const auto* w=source+i.at;
            if(i.op==65 || i.op==66 || i.op==83) {
                valid_id(w[3]);if(root[w[3]] && pointer(w[1])){root[w[2]]=root[w[3]];kind[w[2]]=kind[root[w[3]]];}
            }
        }
        for(const auto& i:instructions) {
            const auto* w=source+i.at;
            if((i.op==245 || i.op==169) && i.count>=3 && pointer(w[1])) {
                for(unsigned j=i.op==245?3:4;j<i.count;j+=(i.op==245?2:1))if(w[j]<bound && root[w[j]])require(false,"unsupported interface pointer merge");
            }
            if(i.op==57)for(unsigned j=4;j<i.count;++j)if(w[j]<bound && root[w[j]])require(false,"interface pointer passed to function");
            if((i.op==63 || i.op==64) && i.count>=3)require(!root.at(w[1]) && !root.at(w[2]),"interface pointer copied as memory");
            if(i.op==67 && i.count>=4)require(!root.at(w[3]),"unsupported interface pointer arithmetic");
            if(i.op==254 && i.count>=2)require(!root.at(w[1]),"interface pointer returned");
        }
    }
    Words specialize() {
        Words prefix,variables,functions;bool function=false;
        for(const auto& i:instructions) {
            const auto* w=source+i.at;if(i.op==54)function=true;
            Words& out=function?functions:prefix;
            if((i.op==5 || i.op==71) && i.count>=2 && w[1]<bound && tint && root[w[1]]==tint)continue;
            if(i.op==15 && tint) {
                unsigned j=3;bool terminated=false;
                for(;j<i.count;++j){auto word=w[j];if(!(word&255) || !(word&0xff00) || !(word&0xff0000) || !(word&0xff000000)){++j;terminated=true;break;}}
                require(terminated,"invalid SPIR-V entry point name");Words entry(w,w+j);
                for(;j<i.count;++j)if(w[j]!=tint)entry.push_back(w[j]);entry[0]=uint32_t(entry.size())<<16|15;out.insert(out.end(),entry.begin(),entry.end());continue;
            }
            if(i.op==59 && !function) {
                if(w[2]==tint)continue;auto start=variables.size();copy(variables,i);
                if(root[w[2]])variables[start+1]=retyped_pointer(w[1],kind[w[2]]);continue;
            }
            if((i.op==65 || i.op==66 || (i.op==83 && pointer(w[1]))) && root[w[2]]) {
                if(root[w[2]]==tint)continue;auto start=out.size();copy(out,i);out[start+1]=retyped_pointer(w[1],kind[w[2]]);continue;
            }
            if(i.op==61 && root[w[3]]) {
                if(root[w[3]]==tint){out.insert(out.end(),{4u<<16|83,w[1],w[2],zero(w[1])});continue;}
                auto type=numeric(w[1],kind[w[3]]),temporary=id();auto start=out.size();copy(out,i);out[start+1]=type;out[start+2]=temporary;
                out.insert(out.end(),{4u<<16|124,w[1],w[2],temporary});continue;
            }
            if(i.op==62 && root[w[1]]) {
                require(root[w[1]]!=tint,"tint buffer is written");auto ptr=result_type[w[1]];require(pointer(ptr),"store pointer has no type");
                auto type=numeric(types[ptr].b,kind[w[1]]),temporary=id();out.insert(out.end(),{4u<<16|124,type,temporary,w[2]});
                auto start=out.size();copy(out,i);out[start+2]=temporary;continue;
            }
            copy(out,i);
        }
        Words result(source,source+5);result[3]=next;
        for(const auto* section:{&prefix,&new_types,&new_constants,&variables,&functions})result.insert(result.end(),section->begin(),section->end());
        return result;
    }
};
inline Words specialize(const uint32_t* words,size_t count,int stage,const uint8_t* kinds){return Module(words,count,stage,kinds).specialize();}
} // namespace shader_interface
