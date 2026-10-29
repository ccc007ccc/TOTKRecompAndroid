// Bounded readers for TotK FSHA9/BNSH 2.2.1, SHARCB9 and RomFS.
// Layouts cross-checked with TotK-Native/tools/shader_{library,archive}.py.
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <zstd.h>

namespace shader_archive {
using Bytes = std::vector<unsigned char>;
inline void require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
struct View {
    const unsigned char *p; size_t n;
    View(const Bytes &b) : p(b.data()), n(b.size()) {}
    View(const unsigned char *p_, size_t n_) : p(p_), n(n_) {}
    View sub(size_t at, size_t size) const {
        require(at <= n && size <= n - at, "truncated shader archive"); return {p + at, size};
    }
    uint32_t u32(size_t at) const { auto s = sub(at, 4); return s.p[0] | uint32_t(s.p[1]) << 8 | uint32_t(s.p[2]) << 16 | uint32_t(s.p[3]) << 24; }
    uint64_t u64(size_t at) const { return u32(at) | uint64_t(u32(at + 4)) << 32; }
    uint16_t u16(size_t at) const { auto s = sub(at, 2); return s.p[0] | uint16_t(s.p[1]) << 8; }
    bool magic(const char *s, size_t len) const { return n >= len && !memcmp(p, s, len); }
    std::string str(size_t at, size_t limit) const {
        auto s = sub(at, limit); const void *end = memchr(s.p, 0, s.n);
        require(end, "unterminated archive name"); return {(const char *)s.p, (const char *)end};
    }
    Bytes bytes() const { return {p, p + n}; }
};
struct Shader {
    uint32_t stage, constants;
    Bytes binary, control;
    // Compute translation starts at the instruction entry. Preserve its original
    // NVN prefix separately for shared-list identities, which hash the full blob.
    Bytes code_prefix;
};
inline Bytes input_identity(const Shader& shader) {
    // Catalog deduplication must preserve the full NVN input, including the
    // compute prefix that translation skips but upstream raw keys still hash.
    auto key=shader.code_prefix;
    key.insert(key.end(),shader.binary.begin(),shader.binary.end());
    key.insert(key.end(),shader.control.begin(),shader.control.end());
    return key;
}
using Emit = std::function<void(Shader)>;
inline void nvn(View binary, View control, uint32_t stage, const Emit &emit) {
    auto fixed_size=control.u32(0x20);
    require(control.n <= 1u<<20 && control.u32(0) == 0x98761234 &&
            (fixed_size==0x880 || (fixed_size==0x7d1 && control.u32(8)==9)) && control.n>=fixed_size,
            "unsupported NVN shader control");
    auto constants = control.u32(0x700), size = control.u32(0x704), entry = control.u32(0x708);
    require(stage == control.u32(0x714) && (stage == 0 || stage == 1 || stage == 5), "unsupported shader stage");
    require(binary.n == size && size <= 16u << 20 && entry == (stage == 5 ? 0x100u : 0x30u) &&
            entry < constants && constants <= size && !(size & 7) && !(constants & 7), "invalid NVN code/constants");
    require(binary.u32(0) == (stage == 5 ? 0x12345679u : 0x12345678u), "NVN stage/code mismatch");
    if (stage != 5) require(constants >= 0x88 && ((binary.u32(entry) >> 10) & 15) == (stage == 0 ? 1u : 5u), "invalid SPH");
    // The renderer passes the NVN prefix for graphics, and the instruction entry for compute.
    auto skip = stage == 5 ? entry : 0;
    // Some library programs append reflection/stream-output data after the
    // fixed header. Keep it in the input identity instead of truncating it.
    emit({stage, constants - skip, binary.sub(skip, size - skip).bytes(), control.bytes(), binary.sub(0, skip).bytes()});
}
inline void bnsh(View b, const Emit &emit) {
    require(b.magic("BNSH\0\0\0\0", 8) && (b.u32(8) == 0x00020201 || b.u32(8) == 0x00020102) && b.u16(12) == 0xfeff &&
            b.u16(20) == 0 && b.u32(28) == b.n && b.sub(0x60,4).magic("grsc",4), "unsupported BNSH");
    auto count = b.u32(0x7c); auto start = b.u64(0x80);
    require(count && count <= 65535 && start == 0xc0, "invalid BNSH variations"); b.sub(start, count * 64ull);
    std::set<uint64_t> seen;
    const uint32_t stages[] = {0, 3, 4, 2, 1, 5};
    for (uint32_t i = 0; i < count; ++i) {
        auto program = b.u64(start + i * 64ull + 16); require(program, "missing BNSH program"); b.sub(program, 144);
        for (unsigned j = 0; j < 6; ++j) {
            auto p = b.u64(program + 8 + j * 8); if (!p || !seen.insert(p).second) continue;
            b.sub(p, 64);
            nvn(b.sub(b.u64(p + 16), b.u32(p + 24)), b.sub(b.u64(p + 8), b.u32(p + 28)), stages[j], emit);
        }
    }
}
inline void bfsha(View b, const Emit &emit) {
    require(b.magic("FSHA    ", 8) && b.u32(8) == 0x00090000 && b.u16(12) == 0xfeff &&
            b.u16(20) == 0 && b.u32(28) == b.n, "unsupported FSHA");
    auto models = b.u64(0x48); auto count = b.u16(0x78);
    require(count && models, "missing FSHA models"); b.sub(models, count * 256ull);
    for (unsigned i = 0; i < count; ++i) {
        auto at = b.u64(models + i * 256ull + 168); require(at, "missing FSHA binary");
        bnsh(b.sub(at, b.u32(at + 28)), emit);
    }
}
inline void sharcb(View b, const Emit &emit) {
    require(b.n >= 0x150 && b.u32(0) == 0x53484142 && b.u32(4) == 9 && b.u32(8) == b.n &&
            b.u32(12) == 5 && b.u32(16) == 0, "unsupported SHARCB");
    auto code = b.u32(28); require(uint64_t(code) + b.u32(24) == b.n && !(code & 4095), "invalid SHARCB code section");
    auto array = 0x150ull + b.u32(20); auto length = b.u32(array), count = b.u32(array + 4);
    require(count && count <= 100000 && length >= 8ull + count * 16ull, "invalid SHARCB array");
    b.sub(array, length); auto end = array + length; require(end <= code, "SHARCB array overlaps code");
    auto cursor = array + 8;
    for (uint32_t i = 0; i < count; ++i) {
        require(cursor <= end && end - cursor >= 16, "truncated SHARCB record");
        auto size = b.u32(cursor), kind = b.u32(cursor + 4);
        require(size >= 16 && size <= end - cursor && kind <= 3, "invalid SHARCB record");
        int64_t payload = int64_t(cursor) + 16 + int32_t(b.u32(cursor + 8));
        auto payload_size = b.u32(cursor + 12);
        require(payload >= int64_t(array + 8) && uint64_t(payload) <= end && payload_size >= 16 &&
                payload_size <= end - payload, "invalid SHARCB payload");
        int64_t control = payload + int32_t(b.u32(payload + 12)); auto control_size = b.u32(payload + 8);
        require(control_size >= 0x718 && control >= int64_t(array + 8) && uint64_t(control) + control_size <= end,
                "invalid SHARCB control");
        auto binary = uint64_t(code) + b.u64(payload); require(binary >= code, "invalid SHARCB offset");
        nvn(b.sub(binary, b.u32(control + 0x704)), b.sub(control, control_size), kind == 3 ? 5 : kind, emit);
        cursor += size;
    }
    require(cursor == end, "SHARCB count mismatch");
}
inline void sarc(View b, const std::function<void(std::string, View)> &emit) {
    require(b.magic("SARC",4) && b.u16(6) == 0xfeff && b.u32(8) == b.n, "unsupported SARC");
    auto fat = b.u16(4); auto data = b.u32(12);
    require(fat >= 20 && b.sub(fat,4).magic("SFAT",4) && b.u16(fat+4) >= 12, "invalid SFAT");
    auto count = b.u16(fat+6); size_t entries = fat + b.u16(fat+4), nt = entries + count * 16ull;
    require(b.sub(nt,4).magic("SFNT",4) && b.u16(nt+4) >= 8, "invalid SFNT");
    size_t names = nt + b.u16(nt+4); require(names <= data && data <= b.n, "invalid SARC names");
    std::set<std::string> seen;
    for (unsigned i=0; i<count; ++i) {
        auto p = entries + i*16; auto flags=b.u32(p+4), start=b.u32(p+8), end=b.u32(p+12);
        require(flags >> 24 == 1 && start <= end && end <= b.n-data, "invalid SARC member");
        size_t at = names + (flags & 0xffffff)*4ull; require(at < data, "invalid SARC name");
        auto name=b.str(at,data-at); require(seen.insert(name).second,"duplicate SARC member");
        emit(name,b.sub(data+start,end-start));
    }
}
enum class ArchiveKind { None, Bnsh, Bfsha, Sharcb, Sarc, Effect };
inline ArchiveKind archive_kind(std::string name) {
    auto ends=[&](const char* suffix){auto n=strlen(suffix);return name.size()>=n && name.compare(name.size()-n,n,suffix)==0;};
    if(ends(".zs"))name.resize(name.size()-3);
    if(ends(".bnsh"))return ArchiveKind::Bnsh;
    if(ends(".bfsha"))return ArchiveKind::Bfsha;
    if(ends(".sharcb"))return ArchiveKind::Sharcb;
    if(ends(".sarc") || ends(".blarc"))return ArchiveKind::Sarc;
    if(ends(".esetb.byml"))return ArchiveKind::Effect;
    return ArchiveKind::None;
}
class Romfs {
    FILE *file = nullptr;
    uint64_t size;
    std::map<std::string,std::pair<uint64_t,uint64_t>> index;
public:
    explicit Romfs(const char *path) {
        file=fopen(path,"rb"); require(file,"cannot open RomFS");
        try {
            require(!fseeko(file,0,SEEK_END),"cannot seek RomFS"); auto end=ftello(file); require(end>=0,"cannot size RomFS"); size=end;
            auto raw=read(0,80); View h(raw); require(h.u64(0)==80,"invalid RomFS header");
            auto dr=read(h.u64(24),h.u64(32)), fr=read(h.u64(56),h.u64(64)); View dirs(dr), files(fr);
            const uint32_t empty=~0u; std::set<uint32_t> ds,fs;
            std::vector<std::pair<uint32_t,std::string>> pending{{0,""}};
            auto name=[](View b,size_t off,size_t fixed,size_t len) {
                auto s=b.sub(off+fixed,len); std::string n((const char*)s.p,s.n);
                require(n!="." && n!=".." && n.find_first_of("/\\") == std::string::npos && n.find('\0')==std::string::npos,"unsafe RomFS name");return n;
            };
            while(!pending.empty()) {
                auto [off,parent]=pending.back(); pending.pop_back(); require(ds.insert(off).second,"cyclic RomFS directory");
                auto n=name(dirs,off,24,dirs.u32(off+20)); require(!n.empty() || !off,"empty RomFS directory");
                auto rel=parent+(n.empty()?"":n+"/");
                auto sibling=dirs.u32(off+4),child=dirs.u32(off+8),f=dirs.u32(off+12);
                if(sibling!=empty)pending.emplace_back(sibling,parent); if(child!=empty)pending.emplace_back(child,rel);
                while(f!=empty) {
                    require(fs.insert(f).second,"cyclic RomFS file"); auto fn=name(files,f,32,files.u32(f+28));require(!fn.empty(),"empty RomFS file");
                    auto at=files.u64(f+8),len=files.u64(f+16),base=h.u64(72);
                    require(base<=size && at<=size-base && len<=size-base-at,"truncated RomFS file");
                    require(index.emplace(rel+fn,std::make_pair(base+at,len)).second,"duplicate RomFS file"); f=files.u32(f+4);
                }
            }
        } catch (...) { fclose(file); file=nullptr; throw; }
    }
    ~Romfs(){if(file)fclose(file);}
    Romfs(const Romfs&)=delete;
    Bytes read(uint64_t at,uint64_t len) {
        require(at<=size && len<=size-at && len<=256u<<20,"RomFS span exceeds bounds");
        require(!fseeko(file,at,SEEK_SET),"cannot seek RomFS file");Bytes out(len);
        require(fread(out.data(),1,out.size(),file)==out.size(),"cannot read RomFS file");return out;
    }
    Bytes read(const std::string &name) {auto it=index.find(name);require(it!=index.end(),"missing shader archive/dictionaries");return read(it->second.first,it->second.second);}
    std::vector<std::string> archives() const {
        // Preserve the original catalog order, then include library and UI
        // archives. The first game draw already uses Lib/agl/agl_common.
        std::vector<std::string> out;
        for(unsigned group=0;group<3;++group)for(const auto& row:index) {
            auto kind=archive_kind(row.first);
            unsigned category=kind==ArchiveKind::Effect?2:row.first.rfind("Shader/",0)==0?0:1;
            if(category==group && kind!=ArchiveKind::None)out.push_back(row.first);
        }
        require(!out.empty(),"no shader archives in RomFS");return out;
    }
    std::vector<std::string> files(const std::string& prefix,const char* suffix) const {
        std::vector<std::string> out;
        for(const auto& row:index)if(row.first.rfind(prefix,0)==0 && ends(row.first,suffix))out.push_back(row.first);
        return out;
    }
    static bool ends(const std::string &s,const char *suffix){auto n=strlen(suffix);return s.size()>=n && s.compare(s.size()-n,n,suffix)==0;}
};
class Decoder {
    std::map<unsigned,Bytes> dictionaries;
    ZSTD_DCtx *ctx = ZSTD_createDCtx();
public:
    Decoder(){require(ctx,"cannot allocate Zstandard decoder");}
    ~Decoder(){ZSTD_freeDCtx(ctx);}
    Bytes decode(const Bytes &input) {
        auto n=ZSTD_getFrameContentSize(input.data(),input.size()); require(n<=256u<<20,"invalid/oversized Zstandard frame");
        require(ZSTD_findFrameCompressedSize(input.data(),input.size())==input.size(),"truncated/trailing Zstandard data");
        auto id=ZSTD_getDictID_fromFrame(input.data(),input.size());auto it=dictionaries.find(id);
        require(!id || it!=dictionaries.end(),"missing Zstandard dictionary"); Bytes out(n);
        auto r=ZSTD_decompress_usingDict(ctx,out.data(),out.size(),input.data(),input.size(),id?it->second.data():nullptr,id?it->second.size():0);
        require(!ZSTD_isError(r) && r==n,"Zstandard decompression failed");return out;
    }
    void init(const Bytes &input) {
        auto raw=decode(input);sarc(View(raw),[&](std::string,View member){
            auto id=ZSTD_getDictID_fromDict(member.p,member.n);require(id && dictionaries.emplace(id,member.bytes()).second,"invalid/duplicate dictionary");
        });
    }
};
// Visit declared shader files and nested SARC members using the same discovery
// rules for the flat audit and the program-link catalog. Unrelated members do
// not become shader inputs; a malformed declared shader still fails preparation.
using ArchiveVisitor=std::function<void(ArchiveKind,const std::string&,View)>;
// Effect resources declare aligned PtclBin data in BYML, with BNSH files inside
// VFXB's GRSN children. Follow their offsets and lengths; do not search arbitrary
// texture/code bytes for a signature or discard the BNSH stage associations.
inline void effect_shaders(View b,const std::string& name,const ArchiveVisitor& visit) {
    require(b.magic("YB\5\0",4),"unsupported effect BYML");
    auto keys=b.u32(4),root=b.u32(12);
    require(keys>=16 && root>=16 && (b.u32(keys)&255)==0xc2 && (b.u32(root)&255)==0xc1,"invalid effect BYML tables");
    auto nk=b.u32(keys)>>8,nr=b.u32(root)>>8;
    b.sub(uint64_t(keys)+4,(uint64_t(nk)+1)*4);b.sub(uint64_t(root)+4,uint64_t(nr)*8);
    View v(nullptr,0);bool found=false;
    for(unsigned i=0;i<nr;++i) {
        uint64_t at=uint64_t(root)+4+uint64_t(i)*8;auto entry=b.u32(at),key=entry&0xffffff;
        require(key<nk,"invalid effect BYML key");
        auto start=b.u32(uint64_t(keys)+4+uint64_t(key)*4),end=b.u32(uint64_t(keys)+8+uint64_t(key)*4);
        require(start>=4+(uint64_t(nk)+1)*4 && end>start,"invalid effect key string range");
        if(b.str(uint64_t(keys)+start,end-start)!="PtclBin")continue;
        require(!found && entry>>24==0xa2,"invalid effect PtclBin");found=true;
        uint64_t data=b.u32(at+4);auto size=b.u32(data),alignment=b.u32(data+4);
        require(alignment && !(alignment&(alignment-1)) && alignment<=1u<<20,"invalid effect binary alignment");
        auto offset=(data+8+alignment-1)&~uint64_t(alignment-1);v=b.sub(offset,size);
    }
    require(found && v.magic("VFXB    ",8) && v.u32(8)==0x00330400 && v.u16(12)==0xfeff &&
            v.u16(0x14)==0 && v.u32(28)==v.n,"unsupported effect VFXB");
    uint64_t at=v.u16(0x16);require(at>=32,"invalid VFXB first block");
    for(unsigned blocks=0;;++blocks) {
        require(blocks<65536,"effect block limit exceeded");auto block=v.sub(at,32);
        if(block.magic("GRSN",4)) {
            auto child=block.u32(8);
            if(child!=UINT32_MAX) {
                require(child>=32,"invalid GRSN child offset");uint64_t node=at+child;
                for(unsigned index=0;;++index) {
                    require(index<65536,"effect shader block limit exceeded");auto row=v.sub(node,32);
                    if(row.magic("GRSR",4) || row.magic("GRSC",4)) {
                        auto offset=row.u32(20);require(offset>=32,"invalid effect BNSH offset");
                        auto shader=v.sub(node+offset,row.u32(4));
                        std::string source=name+":PtclBin/GRSN-"+std::to_string(blocks)+"/"+
                            std::string((const char*)row.p,4)+"-"+std::to_string(index)+".bnsh";
                        visit(ArchiveKind::Bnsh,source,shader);
                    } else {
                        // Graphics/compute resource metadata accompanies GRSR/GRSC.
                        require(row.magic("GRRE",4) || row.magic("GRCE",4) || row.magic("GRRI",4) || row.magic("GRCI",4),
                                "unsupported effect shader block");
                        require(row.u32(8)==UINT32_MAX,"unsupported nested effect resource block");
                        if(row.u32(4)) {require(row.u32(20)>=32,"invalid effect resource data");v.sub(node+row.u32(20),row.u32(4));}
                    }
                    auto next=row.u32(12);if(next==UINT32_MAX)break;
                    require(next>=32,"invalid effect shader sibling offset");node+=next;
                }
            }
        }
        auto next=block.u32(12);if(next==UINT32_MAX)break;
        require(next>=32,"invalid effect block sibling offset");at+=next;
    }
}
inline void visit_archive(View b,const std::string& name,Decoder& decoder,const ArchiveVisitor& visit,unsigned depth=0) {
    require(depth<=8,"shader archive nesting limit exceeded");
    auto kind=archive_kind(name);if(kind==ArchiveKind::None)return;
    Bytes decoded;
    if(Romfs::ends(name,".zs")){decoded=decoder.decode(b.bytes());b=View(decoded);}
    if(kind==ArchiveKind::Sarc) {
        sarc(b,[&](std::string member,View v){visit_archive(v,name+":"+member,decoder,visit,depth+1);});
    } else if(kind==ArchiveKind::Effect) {
        try {effect_shaders(b,name,visit);}
        catch(const std::exception& e){throw std::runtime_error(name+": "+e.what());}
    } else {
        try {visit(kind,name,b);}
        catch(const std::exception& e){throw std::runtime_error(name+": "+e.what());}
    }
}
inline void inventory(Romfs &rom, const Emit &emit, const std::function<void(const std::string&)> &progress) {
    Decoder decoder; decoder.init(rom.read("Pack/ZsDic.pack.zs"));
    for(auto &name:rom.archives()) {
        progress(name);auto bytes=rom.read(name);
        visit_archive(View(bytes),name,decoder,[&](ArchiveKind kind,const std::string&,View b){
            if(kind==ArchiveKind::Bnsh)bnsh(b,emit);
            else if(kind==ArchiveKind::Bfsha)bfsha(b,emit);
            else sharcb(b,emit);
        });
    }
}
} // namespace shader_archive
