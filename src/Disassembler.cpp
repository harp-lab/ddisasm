//===- Disassembler.cpp -----------------------------------------*- C++ -*-===//
//
//  Copyright (C) 2019 GrammaTech, Inc.
//
//  This code is licensed under the GNU Affero General Public License
//  as published by the Free Software Foundation, either version 3 of
//  the License, or (at your option) any later version. See the
//  LICENSE.txt file in the project root for license terms or visit
//  https://www.gnu.org/licenses/agpl.txt.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
//  GNU Affero General Public License for more details.
//
//  This project is sponsored by the Office of Naval Research, One Liberty
//  Center, 875 N. Randolph Street, Arlington, VA 22203 under contract #
//  N68335-17-C-0700.  The content of the information does not necessarily
//  reflect the position or policy of the Government and no official
//  endorsement should be inferred.
//
//===----------------------------------------------------------------------===//

#include "Disassembler.h"

#include <LIEF/LIEF.hpp>
#include <boost/uuid/uuid_generators.hpp>

#include "AuxDataSchema.h"
#include "gtirb-decoder/CompositeLoader.h"

using ImmOp = relations::ImmOp;
using IndirectOp = relations::IndirectOp;

souffle::tuple &operator>>(souffle::tuple &t, gtirb::Addr &ea)
{
    uint64_t x;
    t >> x;
    ea = gtirb::Addr(x);
    return t;
}

souffle::tuple &operator>>(souffle::tuple &t, uint8_t &byte)
{
    int64_t x;
    t >> x;
    assert(x >= 0);
    assert(x < 256);
    byte = static_cast<uint8_t>(x);
    return t;
}

struct DecodedInstruction
{
    gtirb::Addr EA;
    uint64_t Size;
    std::map<uint64_t, std::variant<ImmOp, IndirectOp>> Operands;
    uint64_t immediateOffset;
    uint64_t displacementOffset;
};

std::map<gtirb::Addr, DecodedInstruction> recoverInstructions(souffle::SouffleProgram *prog)
{
    std::map<uint64_t, ImmOp> Immediates;
    for(auto &output : *prog->getRelation("op_immediate"))
    {
        uint64_t operandCode;
        ImmOp immediate;
        output >> operandCode >> immediate;
        Immediates[operandCode] = immediate;
    };
    std::map<uint64_t, IndirectOp> Indirects;
    for(auto &output : *prog->getRelation("op_indirect"))
    {
        uint64_t operandCode, size;
        IndirectOp indirect;
        output >> operandCode >> indirect.Reg1 >> indirect.Reg2 >> indirect.Reg3 >> indirect.Mult
            >> indirect.Disp >> size;
        Indirects[operandCode] = indirect;
    };
    std::map<gtirb::Addr, DecodedInstruction> insns;
    for(auto &output : *prog->getRelation("instruction"))
    {
        DecodedInstruction insn;
        gtirb::Addr EA;
        std::string prefix, opcode;
        output >> EA >> insn.Size >> prefix >> opcode;
        for(size_t i = 1; i <= 4; i++)
        {
            uint64_t operandIndex;
            output >> operandIndex;
            auto foundImmediate = Immediates.find(operandIndex);
            if(foundImmediate != Immediates.end())
                insn.Operands[i] = foundImmediate->second;
            else
            {
                auto foundIndirect = Indirects.find(operandIndex);
                if(foundIndirect != Indirects.end())
                    insn.Operands[i] = foundIndirect->second;
            }
        }
        output >> insn.immediateOffset >> insn.displacementOffset;
        insns[EA] = insn;
    }
    return insns;
}

struct CodeInBlock
{
    CodeInBlock(souffle::tuple &tuple)
    {
        assert(tuple.size() == 2);
        tuple >> EA >> BlockAddress;
    };

    gtirb::Addr EA{0};
    gtirb::Addr BlockAddress{0};
};

struct BlockInformation
{
    BlockInformation(gtirb::Addr ea) : EA(ea)
    {
    }

    BlockInformation(souffle::tuple &tuple)
    {
        assert(tuple.size() == 3);
        tuple >> EA >> size;
    };

    gtirb::Addr EA{0};
    uint64_t size{0};
};

struct PLTReference
{
    PLTReference(gtirb::Addr ea) : EA(ea)
    {
    }

    PLTReference(souffle::tuple &tuple)
    {
        assert(tuple.size() == 2);
        tuple >> EA >> Name;
    };

    std::string Name;
    gtirb::Addr EA{0};
};

struct BlockPoints
{
    BlockPoints(souffle::tuple &tuple)
    {
        assert(tuple.size() == 4);
        tuple >> Address >> Predecessor >> Importance >> Why;
    };

    gtirb::Addr Address{0};
    gtirb::Addr Predecessor{0};
    int64_t Importance{0};
    std::string Why;
};

template <typename T>
using VectorByEA = boost::multi_index_container<
    T, boost::multi_index::indexed_by<boost::multi_index::ordered_non_unique<
           boost::multi_index::member<T, decltype(T::EA), &T::EA>>>>;

struct SymbolicExpr
{
    SymbolicExpr(gtirb::Addr ea) : EA(ea)
    {
    }

    SymbolicExpr(souffle::tuple &tuple)
    {
        assert(tuple.size() == 4);
        tuple >> EA >> Size >> Symbol >> Addend;
    };

    gtirb::Addr EA{0};
    uint64_t Size{0};
    std::string Symbol;
    int64_t Addend{0};
};

struct SymExprSymbolMinusSymbol
{
    SymExprSymbolMinusSymbol(gtirb::Addr ea) : EA(ea)
    {
    }

    SymExprSymbolMinusSymbol(souffle::tuple &tuple)
    {
        assert(tuple.size() == 6);

        tuple >> EA >> Size >> Symbol1 >> Symbol2 >> Scale >> Offset;
    };

    gtirb::Addr EA{0};
    uint64_t Size;
    std::string Symbol1;
    std::string Symbol2;
    uint64_t Scale;
    int64_t Offset;
};

struct SymbolicExprAttribute
{
    explicit SymbolicExprAttribute(gtirb::Addr A) : EA(A)
    {
    }
    explicit SymbolicExprAttribute(souffle::tuple &T)
    {
        assert(T.size() == 2);
        T >> EA >> Type;
    }
    gtirb::Addr EA{0};
    std::string Type{"NONE"};
};

struct StringDataObject
{
    StringDataObject(gtirb::Addr A) : EA(A)
    {
    }

    StringDataObject(souffle::tuple &T)
    {
        assert(T.size() == 3);
        T >> EA >> End >> Encoding;
    };

    gtirb::Addr EA{0};
    gtirb::Addr End{0};
    std::string Encoding{"NONE"};
};

struct SymbolSpecialType
{
    SymbolSpecialType(gtirb::Addr ea) : EA(ea)
    {
    }

    SymbolSpecialType(souffle::tuple &tuple)
    {
        assert(tuple.size() == 2);

        tuple >> EA >> Type;
    };

    gtirb::Addr EA{0};
    std::string Type;
};

struct SymbolicInfo
{
    VectorByEA<SymbolicExpr> SymbolicExprs;
    VectorByEA<SymExprSymbolMinusSymbol> SymbolMinusSymbolSymbolicExprs;
    VectorByEA<SymbolicExprAttribute> SymbolicExprAttributes;
};

template <typename T>
std::vector<T> convertRelation(const std::string &relation, souffle::SouffleProgram *prog)
{
    std::vector<T> result;
    for(auto &output : *prog->getRelation(relation))
    {
        result.emplace_back(output);
    }
    return result;
}

template <typename Container, typename Elem = typename Container::value_type>
Container convertSortedRelation(const std::string &relation, souffle::SouffleProgram *prog)
{
    Container result;
    for(auto &output : *prog->getRelation(relation))
    {
        Elem elem(output);
        result.insert(elem);
    }
    return result;
}

template <>
std::set<gtirb::Addr> convertSortedRelation<std::set<gtirb::Addr>>(const std::string &relation,
                                                                   souffle::SouffleProgram *prog)
{
    std::set<gtirb::Addr> result;
    for(auto &output : *prog->getRelation(relation))
    {
        result.insert(gtirb::Addr(output[0]));
    };
    return result;
}

static std::string getLabel(uint64_t ea)
{
    std::stringstream ss;
    ss << ".L_" << std::hex << ea;
    return ss.str();
}

std::string stripSymbolVersion(const std::string Name)
{
    if(size_t I = Name.find('@'); I != std::string::npos)
    {
        return Name.substr(0, I);
    }
    return Name;
}

void removeSectionSymbols(gtirb::Context &Context, gtirb::Module &Module)
{
    auto *SymbolInfo = Module.getAuxData<gtirb::schema::ElfSymbolInfo>();
    if(!SymbolInfo)
    {
        return;
    }
    std::vector<gtirb::UUID> Remove;
    for(const auto &[Uuid, Info] : *SymbolInfo)
    {
        if(std::get<1>(Info) == "SECTION")
        {
            Remove.push_back(Uuid);
        }
    }
    for(const auto Uuid : Remove)
    {
        gtirb::Node *N = gtirb::Node::getByUUID(Context, Uuid);
        if(auto *Symbol = dyn_cast_or_null<gtirb::Symbol>(N))
        {
            Module.removeSymbol(Symbol);
            SymbolInfo->erase(Uuid);
        }
    }
}

void buildSymbolVersions(gtirb::Module &Module)
{
    if(Module.getFileFormat() != gtirb::FileFormat::ELF)
    {
        return;
    }

    std::map<gtirb::UUID, std::string> SymbolVersions;

    std::vector<std::tuple<gtirb::Symbol *, std::string, std::string>> Versioned;
    for(auto &Symbol : Module.symbols())
    {
        const std::string &Name = Symbol.getName();
        if(size_t I = Name.find('@'); I != std::string::npos)
        {
            Versioned.push_back({&Symbol, Name.substr(0, I), Name.substr(I)});
        }
    }
    for(auto [Symbol, Name, Version] : Versioned)
    {
        Symbol->setName(Name);
        SymbolVersions.insert({Symbol->getUUID(), Version});
    }

    Module.addAuxData<gtirb::schema::ElfSymbolVersions>(std::move(SymbolVersions));
}

void buildInferredSymbols(gtirb::Context &Context, gtirb::Module &Module,
                          souffle::SouffleProgram *Prog)
{
    auto *SymbolInfo = Module.getAuxData<gtirb::schema::ElfSymbolInfo>();
    auto *SymbolTabIdxInfo = Module.getAuxData<gtirb::schema::ElfSymbolTabIdxInfo>();
    for(auto &T : *Prog->getRelation("inferred_symbol_name"))
    {
        gtirb::Addr Addr;
        std::string Name;
        std::string Scope, Type;
        T >> Addr >> Name >> Scope >> Type;

        // Replace decimal address with hex.
        if(Name.find("FUN_") == 0)
        {
            std::stringstream S;
            S << "FUN_" << std::hex << static_cast<uint64_t>(Addr);
            if(Name.rfind("_IFUNC") != std::string::npos)
            {
                S << "_IFUNC";
            }
            Name = S.str();
        }
        if(Name.find(".L_DEC_") == 0)
        {
            std::stringstream S;
            S << ".L_" << std::hex << static_cast<uint64_t>(Addr);
            Name = S.str();
        }

        if(!Module.findSymbols(Name))
        {
            gtirb::Symbol *Symbol = Module.addSymbol(Context, Addr, Name);
            if(SymbolInfo)
            {
                auxdata::ElfSymbolInfo Info = {0, Type, Scope, "DEFAULT", 0};
                SymbolInfo->insert({Symbol->getUUID(), Info});
            }
            if(SymbolTabIdxInfo)
            {
                auxdata::ElfSymbolTabIdxInfo TabIdx =
                    std::vector<std::tuple<std::string, uint64_t>>();
                SymbolTabIdxInfo->insert({Symbol->getUUID(), TabIdx});
            }
        }
    }
    // Rename ARM mapping symbols.
    std::vector<gtirb::Symbol *> MappingSymbols;
    for(auto &Symbol : Module.symbols())
    {
        std::string Name = Symbol.getName();
        if(Name == "$a" || Name == "$d" || Name.substr(0, 2) == "$t" || Name == "$x")
        {
            MappingSymbols.push_back(&Symbol);
        }
    }
    for(auto *Symbol : MappingSymbols)
    {
        if(std::optional<gtirb::Addr> A = Symbol->getAddress())
        {
            Symbol->setName(getLabel(static_cast<uint64_t>(*A)));
        }
    }
}

// auxiliary function to get a symbol with an address and name
gtirb::Symbol *findSymbol(gtirb::Module &module, gtirb::Addr ea, std::string name)
{
    auto found = module.findSymbols(ea);
    for(gtirb::Symbol &symbol : found)
    {
        if(symbol.getName() == name)
            return &symbol;
    }
    return nullptr;
}

// auxiliary function to get a symbol with an address and name
gtirb::Symbol *findFirstSymbol(gtirb::Module &module, std::string Name)
{
    static std::string inferred_prefix(".L_DEC_");
    static std::string fun_prefix("FUN_");
    if(Name.find(inferred_prefix) == 0)
    {
        std::stringstream S;
        S << ".L_" << std::hex << std::stoul(Name.substr(inferred_prefix.size()));
        Name = S.str();
    }
    else if(Name.find(fun_prefix) == 0)
    {
        std::stringstream S;
        S << "FUN_" << std::hex << std::stoul(Name.substr(fun_prefix.size()));
        Name = S.str();
    }
    auto found = module.findSymbols(Name);
    if(found.begin() == found.end())
    {
        std::cerr << "Missing symbol: " << Name << std::endl;
        exit(1);
    }
    return &*found.begin();
}

// Build a first version of the SymbolForwarding table with copy relocations and
// other ABI-specific artifacts that may be duplicated or reintroduced during
// reassembly.
void buildSymbolForwarding(gtirb::Context &Context, gtirb::Module &Module,
                           souffle::SouffleProgram *Prog)
{
    std::map<gtirb::UUID, gtirb::UUID> SymbolForwarding;
    for(auto &T : *Prog->getRelation("relocation_alias"))
    {
        gtirb::Addr EA;
        std::string Name;
        std::string Alias;
        T >> EA >> Alias >> Name;
        gtirb::Symbol *Symbol = findSymbol(Module, EA, Alias);
        if(Symbol)
        {
            // Create orphaned symbol for OBJECT copy relocation aliases.
            Alias = stripSymbolVersion(Alias);
            gtirb::Symbol *NewSymbol = Module.addSymbol(Context, EA, Alias + "_copy");
            Symbol->setReferent(Module.addProxyBlock(Context));
            SymbolForwarding[NewSymbol->getUUID()] = Symbol->getUUID();
        }
    }
    for(auto &T : *Prog->getRelation("relocation"))
    {
        gtirb::Addr EA;
        int64_t Offset;
        std::string Type, Name;
        T >> EA >> Type >> Name >> Offset;
        if(Type == "COPY")
        {
            gtirb::Symbol *CopySymbol = findSymbol(Module, EA, Name);
            if(CopySymbol)
            {
                gtirb::Symbol *RealSymbol = Module.addSymbol(Context, Name);
                RealSymbol->setReferent(Module.addProxyBlock(Context));
                Name = stripSymbolVersion(Name);
                CopySymbol->setName(Name + "_copy");
                SymbolForwarding[CopySymbol->getUUID()] = RealSymbol->getUUID();
            }
        }
    }
    for(auto &T : *Prog->getRelation("abi_intrinsic"))
    {
        gtirb::Addr EA;
        std::string Name;
        T >> EA >> Name;

        gtirb::Symbol *Symbol = findSymbol(Module, EA, Name);
        if(Symbol)
        {
            gtirb::Symbol *NewSymbol = Module.addSymbol(Context, Name);
            // Create orphaned symbol for OBJECT copy relocation aliases.
            Name = stripSymbolVersion(Name);
            Symbol->setName(Name + "_copy");
            SymbolForwarding[Symbol->getUUID()] = NewSymbol->getUUID();
        }
    }
    Module.addAuxData<gtirb::schema::SymbolForwarding>(std::move(SymbolForwarding));
}

gtirb::SymAttributeSet buildSymbolicExpressionAttributes(
    gtirb::Addr EA, const VectorByEA<SymbolicExprAttribute> &SymbolicDataAttributes)
{
    const static std::map<std::string, gtirb::SymAttribute> AttributeMap = {
        {"Part0", gtirb::SymAttribute::Part0},
        {"Part1", gtirb::SymAttribute::Part1},
        {"Part2", gtirb::SymAttribute::Part2},
        {"Part3", gtirb::SymAttribute::Part3},
        {"AddrRelGot", gtirb::SymAttribute::AddrRelGot},
        {"GotRef", gtirb::SymAttribute::GotRef},
        {"GotRelPC", gtirb::SymAttribute::GotRelPC},
        {"GotRelGot", gtirb::SymAttribute::GotRelGot},
        {"GotRelAddr", gtirb::SymAttribute::GotRelAddr},
        {"GotPage", gtirb::SymAttribute::GotPage},
        {"GotPageOfst", gtirb::SymAttribute::GotPageOfst},
        {"PltRef", gtirb::SymAttribute::PltRef},
        {"TpOff", gtirb::SymAttribute::TpOff},
        {"TlsLd", gtirb::SymAttribute::TlsLd},
        {"TlsGd", gtirb::SymAttribute::TlsGd},
        {"GotOff", gtirb::SymAttribute::GotOff},
        {"NtpOff", gtirb::SymAttribute::NtpOff},
        {"DtpOff", gtirb::SymAttribute::DtpOff},
        {"Lo12", gtirb::SymAttribute::Lo12},
    };
    gtirb::SymAttributeSet Attributes;

    auto Range = SymbolicDataAttributes.equal_range(EA);
    for(auto It = Range.first; It != Range.second; It++)
    {
        Attributes.addFlag(AttributeMap.at(It->Type));
    }

    return Attributes;
}

bool isNullReg(const std::string &reg)
{
    return reg == "NONE";
}

// Expand the SymbolForwarding table with plt references
void expandSymbolForwarding(gtirb::Context &context, gtirb::Module &module,
                            souffle::SouffleProgram *prog)
{
    auto *symbolForwarding = module.getAuxData<gtirb::schema::SymbolForwarding>();
    for(auto &output : *prog->getRelation("plt_block"))
    {
        gtirb::Addr ea;
        std::string name;
        output >> ea >> name;
        // the inference of plt_block guarantees that there is at most one
        // destination symbol for each source
        auto foundSrc = module.findSymbols(ea);
        auto foundDest = module.findSymbols(name);
        for(gtirb::Symbol &src : foundSrc)
        {
            for(gtirb::Symbol &dest : foundDest)
            {
                (*symbolForwarding)[src.getUUID()] = dest.getUUID();
            }
        }
    }
    for(auto &output : *prog->getRelation("got_reference"))
    {
        gtirb::Addr ea;
        std::string name;
        output >> ea >> name;
        auto foundSrc = module.findSymbols(ea);
        gtirb::Symbol *Dest = findFirstSymbol(module, name);
        for(gtirb::Symbol &src : foundSrc)
        {
            (*symbolForwarding)[src.getUUID()] = Dest->getUUID();
        }
    }
}

template <class ExprType, typename... Args>
void addSymbolicExpressionToCodeBlock(gtirb::Module &Module, gtirb::Addr Addr, uint64_t Size,
                                      Args... A)
{
    if(auto it = Module.findCodeBlocksOn(Addr); !it.empty())
    {
        gtirb::CodeBlock &Block = *it.begin();
        gtirb::ByteInterval *ByteInterval = Block.getByteInterval();
        std::optional<gtirb::Addr> BaseAddr = ByteInterval->getAddress();
        assert(BaseAddr && "Found byte interval without address.");
        uint64_t BlockOffset = static_cast<uint64_t>(Addr - *BaseAddr);
        ByteInterval->addSymbolicExpression<ExprType>(BlockOffset, A...);
        if(auto *Sizes = Module.getAuxData<gtirb::schema::SymbolicExpressionSizes>())
        {
            gtirb::Offset ExpressionOffset = gtirb::Offset(ByteInterval->getUUID(), BlockOffset);
            (*Sizes)[ExpressionOffset] = Size;
        }
    }
}

void buildSymbolicExpr(gtirb::Module &Module, const gtirb::Addr &Ea,
                       const SymbolicInfo &SymbolicInfo)
{
    gtirb::SymAttributeSet attrs =
        buildSymbolicExpressionAttributes(Ea, SymbolicInfo.SymbolicExprAttributes);
    // Symbolic expression from relocation
    if(const auto SymExpr = SymbolicInfo.SymbolicExprs.find(Ea);
       SymExpr != SymbolicInfo.SymbolicExprs.end())
    {
        gtirb::Symbol *FoundSymbol = findFirstSymbol(Module, SymExpr->Symbol);
        // FIXME: We need to handle overlapping sections here.
        addSymbolicExpressionToCodeBlock<gtirb::SymAddrConst>(Module, Ea, SymExpr->Size,
                                                              SymExpr->Addend, FoundSymbol, attrs);
        return;
    }
    // Symbol-Symbol case
    if(const auto SymExpr = SymbolicInfo.SymbolMinusSymbolSymbolicExprs.find(Ea);
       SymExpr != SymbolicInfo.SymbolMinusSymbolSymbolicExprs.end())
    {
        gtirb::Symbol *FoundSymbol1 = findFirstSymbol(Module, SymExpr->Symbol1);
        gtirb::Symbol *FoundSymbol2 = findFirstSymbol(Module, SymExpr->Symbol2);
        addSymbolicExpressionToCodeBlock<gtirb::SymAddrAddr>(Module, Ea, SymExpr->Size,
                                                             SymExpr->Scale, SymExpr->Offset,
                                                             FoundSymbol1, FoundSymbol2, attrs);
        return;
    }
}

void buildCodeSymbolicInformation(gtirb::Context &context, gtirb::Module &module,
                                  souffle::SouffleProgram *prog)
{
    auto codeInBlock = convertRelation<CodeInBlock>("code_in_refined_block", prog);
    SymbolicInfo symbolicInfo{
        convertSortedRelation<VectorByEA<SymbolicExpr>>("symbolic_expr", prog),
        convertSortedRelation<VectorByEA<SymExprSymbolMinusSymbol>>(
            "symbolic_expr_symbol_minus_symbol", prog),
        convertSortedRelation<VectorByEA<SymbolicExprAttribute>>("symbolic_expr_attribute", prog)};
    std::map<gtirb::Addr, DecodedInstruction> decodedInstructions = recoverInstructions(prog);

    for(auto &cib : codeInBlock)
    {
        const auto inst = decodedInstructions.find(cib.EA);
        assert(inst != decodedInstructions.end());
        for(auto &op : inst->second.Operands)
        {
            if(std::get_if<ImmOp>(&op.second))
                buildSymbolicExpr(module, gtirb::Addr(inst->first + inst->second.immediateOffset),
                                  symbolicInfo);
            if(std::get_if<IndirectOp>(&op.second))
                buildSymbolicExpr(module,
                                  gtirb::Addr(inst->first + inst->second.displacementOffset),
                                  symbolicInfo);
        }
    }
}

void buildCodeBlocks(gtirb::Context &context, gtirb::Module &module, souffle::SouffleProgram *prog)
{
    auto blockInformation =
        convertSortedRelation<VectorByEA<BlockInformation>>("block_information", prog);
    for(auto &output : *prog->getRelation("refined_block"))
    {
        gtirb::Addr blockAddress;
        output >> blockAddress;
        if(auto sections = module.findSectionsOn(blockAddress); !sections.empty())
        {
            gtirb::Section &section = *sections.begin();
            uint64_t size = blockInformation.find(blockAddress)->size;
            if(auto it = section.findByteIntervalsOn(blockAddress); !it.empty())
            {
                if(gtirb::ByteInterval &byteInterval = *it.begin(); byteInterval.getAddress())
                {
                    uint64_t blockOffset = blockAddress - *byteInterval.getAddress();
                    uint64_t isThumb = static_cast<uint64_t>(blockAddress) & 1;
                    byteInterval.addBlock<gtirb::CodeBlock>(context, blockOffset, size, isThumb);
                }
            }
        }
    }
}

// Create DataObjects for labeled objects in the BSS sections, without adding
// data to the ImageByteMap.

void buildBSS(gtirb::Context &context, gtirb::Module &module, souffle::SouffleProgram *prog)
{
    auto bssData = convertSortedRelation<std::set<gtirb::Addr>>("bss_data", prog);
    for(auto &output : *prog->getRelation("bss_section"))
    {
        std::string sectionName;
        output >> sectionName;
        const auto bss_sections = module.findSections(sectionName);
        for(const auto &bss_section : bss_sections)
        {
            // for each bss section we divide in data objects according to the bss_data markers that
            // fall within the range of the section
            auto beginning = bssData.lower_bound(bss_section.getAddress().value());
            // end points to the address at the end of the bss section
            auto end = bssData.lower_bound(*addressLimit(bss_section));
            for(auto i = beginning; i != end; ++i)
            {
                auto next = i;
                next++;
                if(auto it = module.findByteIntervalsOn(*i); !it.empty())
                {
                    gtirb::ByteInterval &byteInterval = *it.begin();
                    uint64_t blockOffset = *i - byteInterval.getAddress().value();
                    byteInterval.addBlock<gtirb::DataBlock>(context, blockOffset,
                                                            static_cast<uint64_t>(*next - *i));
                }
            }
        }
    }
}

void buildDataBlocks(gtirb::Context &context, gtirb::Module &module, souffle::SouffleProgram *prog)
{
    auto symbolicExprs = convertSortedRelation<VectorByEA<SymbolicExpr>>("symbolic_expr", prog);
    auto symbolMinusSymbol = convertSortedRelation<VectorByEA<SymExprSymbolMinusSymbol>>(
        "symbolic_expr_symbol_minus_symbol", prog);

    auto DataStrings = convertSortedRelation<VectorByEA<StringDataObject>>("string", prog);
    auto symbolSpecialTypes =
        convertSortedRelation<VectorByEA<SymbolSpecialType>>("symbol_special_encoding", prog);
    auto DataBoundary = convertSortedRelation<std::set<gtirb::Addr>>("data_object_boundary", prog);
    auto symbolicDataAttributes =
        convertSortedRelation<VectorByEA<SymbolicExprAttribute>>("symbolic_expr_attribute", prog);

    std::map<gtirb::UUID, std::string> typesTable;

    std::map<gtirb::Offset, uint64_t> SymbolicSizes;

    for(auto &output : *prog->getRelation("initialized_data_segment"))
    {
        gtirb::Addr begin, end;
        output >> begin >> end;
        // we don't create data blocks that exceed the data segment
        DataBoundary.insert(end);
        for(auto currentAddr = begin; currentAddr < end;
            /*incremented in each case*/)
        {
            gtirb::DataBlock *d;
            if(auto it = module.findByteIntervalsOn(currentAddr); !it.empty())
            {
                if(gtirb::ByteInterval &byteInterval = *it.begin(); byteInterval.getAddress())
                {
                    // do not cross byte intervals.
                    DataBoundary.insert(*byteInterval.getAddress() + byteInterval.getSize());
                    uint64_t blockOffset = currentAddr - *byteInterval.getAddress();
                    gtirb::Offset Offset = gtirb::Offset(byteInterval.getUUID(), blockOffset);

                    // symbolic expression created from relocation
                    if(const auto symExpr = symbolicExprs.find(currentAddr);
                       symExpr != symbolicExprs.end())
                    {
                        d = gtirb::DataBlock::Create(context, symExpr->Size);
                        gtirb::Symbol *foundSymbol = findFirstSymbol(module, symExpr->Symbol);

                        byteInterval.addSymbolicExpression<gtirb::SymAddrConst>(
                            blockOffset, symExpr->Addend, foundSymbol);
                        SymbolicSizes[Offset] = symExpr->Size;
                    }
                    else if(const auto SymExprSymMinusSym = symbolMinusSymbol.find(currentAddr);
                            SymExprSymMinusSym != symbolMinusSymbol.end())
                    {
                        d = gtirb::DataBlock::Create(context, SymExprSymMinusSym->Size);
                        gtirb::Symbol *Sym1 = findFirstSymbol(module, SymExprSymMinusSym->Symbol1);
                        gtirb::Symbol *Sym2 = findFirstSymbol(module, SymExprSymMinusSym->Symbol2);
                        byteInterval.addSymbolicExpression<gtirb::SymAddrAddr>(
                            blockOffset, static_cast<int64_t>(SymExprSymMinusSym->Scale),
                            SymExprSymMinusSym->Offset, Sym2, Sym1);
                        SymbolicSizes[Offset] = SymExprSymMinusSym->Size;
                    }
                    else
                        // string
                        if(const auto S = DataStrings.find(currentAddr); S != DataStrings.end())
                    {
                        d = gtirb::DataBlock::Create(context, S->End - currentAddr);
                        typesTable[d->getUUID()] = S->Encoding;
                    }
                    else
                    {
                        // Accumulate region with no symbols into a single DataBlock.
                        auto NextDataObject = DataBoundary.lower_bound(currentAddr + 1);
                        d = gtirb::DataBlock::Create(context, *NextDataObject - currentAddr);
                    }
                    // symbol special types
                    const auto specialType = symbolSpecialTypes.find(currentAddr);
                    if(specialType != symbolSpecialTypes.end())
                        typesTable[d->getUUID()] = specialType->Type;
                    byteInterval.addBlock(blockOffset, d);
                    currentAddr += d->getSize();
                }
            }
        }
    }
    buildBSS(context, module, prog);
    module.addAuxData<gtirb::schema::Encodings>(std::move(typesTable));
    module.addAuxData<gtirb::schema::SymbolicExpressionSizes>(std::move(SymbolicSizes));
}

gtirb::Section *findSectionByIndex(gtirb::Context &C, gtirb::Module &M, uint64_t Index)
{
    auto *SectionIndex = M.getAuxData<gtirb::schema::ElfSectionIndex>();
    if(!SectionIndex)
    {
        std::cerr << "ERROR: Missing `elfSectionIndex' AuxData table\n";
        return nullptr;
    }
    if(auto It = SectionIndex->find(Index); It != SectionIndex->end())
    {
        gtirb::Node *N = gtirb::Node::getByUUID(C, It->second);
        if(auto *Section = dyn_cast_or_null<gtirb::Section>(N))
        {
            return Section;
        }
    }
    return nullptr;
}

void connectSymbolsToBlocks(gtirb::Context &Context, gtirb::Module &Module)
{
    auto *Alignment = Module.getAuxData<gtirb::schema::Alignment>();
    auto *SymbolInfo = Module.getAuxData<gtirb::schema::ElfSymbolInfo>();

    // Assign communal symbols (.comm) to ProxyBlocks; communal variables are
    // allocated by the linker and are not initialized.
    if(SymbolInfo)
    {
        for(auto [Uuid, Info] : *SymbolInfo)
        {
            uint64_t SectionIndex = std::get<4>(Info);
            if(SectionIndex == static_cast<uint64_t>(LIEF::ELF::SYMBOL_SECTION_INDEX::SHN_COMMON))
            {
                gtirb::Node *Node = gtirb::Node::getByUUID(Context, Uuid);
                if(auto *Symbol = dyn_cast_or_null<gtirb::Symbol>(Node);
                   Symbol && Symbol->getAddress())
                {
                    // Alignment is stored in the symbol's value field.
                    if(Alignment)
                    {
                        (*Alignment)[Uuid] = static_cast<uint64_t>(*Symbol->getAddress());
                    }
                    Symbol->setReferent(Module.addProxyBlock(Context));
                }
            }
        }
    }

    std::map<gtirb::Symbol *, std::tuple<gtirb::Node *, bool>> ConnectToBlock;
    for(auto &Symbol : Module.symbols_by_addr())
    {
        if(Symbol.getAddress())
        {
            gtirb::Addr Addr = *Symbol.getAddress();
            if(auto It = Module.findCodeBlocksAt(Addr); !It.empty())
            {
                gtirb::CodeBlock &Block = It.front();
                assert(Addr == *Block.getAddress());
                ConnectToBlock[&Symbol] = {&Block, false};
                continue;
            }
            if(auto It = Module.findDataBlocksAt(Addr); !It.empty())
            {
                gtirb::DataBlock &Block = It.front();
                assert(Addr == *Block.getAddress());
                ConnectToBlock[&Symbol] = {&Block, false};
                continue;
            }
            if(auto It = Module.findCodeBlocksOn(Addr); !It.empty())
            {
                gtirb::CodeBlock &Block = It.front();
                if(Addr > *Block.getAddress())
                {
                    std::cerr << "WARNING: Found integral symbol pointing into existing block:"
                              << Symbol.getName() << std::endl;
                    continue;
                }
            }
            if(auto It = Module.findDataBlocksOn(Addr); !It.empty())
            {
                gtirb::DataBlock &Block = It.front();
                if(Addr > *Block.getAddress())
                {
                    std::cerr << "WARNING: Found integral symbol pointing into existing block: "
                              << Symbol.getName() << std::endl;
                    continue;
                }
            }
            if(auto It = Module.findSectionsOn(Addr - 1); !It.empty())
            {
                if(gtirb::Section &Section = It.front(); Section.getAddress() && Section.getSize())
                {
                    // Symbol points to byte immediately following section.
                    if(Addr == (*Section.getAddress() + *Section.getSize()))
                    {
                        if(auto BlockIt = Section.findBlocksOn(Addr - 1); !BlockIt.empty())
                        {
                            gtirb::Node &Block = BlockIt.front();
                            ConnectToBlock[&Symbol] = {&Block, true};
                            continue;
                        }
                    }
                }
            }
            if(SymbolInfo && SymbolInfo->count(Symbol.getUUID()) > 0)
            {
                auxdata::ElfSymbolInfo Info = (*SymbolInfo)[Symbol.getUUID()];
                uint64_t SectionIndex = std::get<4>(Info);
                if(gtirb::Section *Section = findSectionByIndex(Context, Module, SectionIndex);
                   Section && Section->getAddress() && Section->getSize())
                {
                    if(Addr < Section->getAddress())
                    {
                        if(auto It = Section->blocks(); !It.empty())
                        {
                            std::cerr << "WARNING: Moving symbol to first block of section: "
                                      << Symbol.getName() << std::endl;
                            ConnectToBlock[&Symbol] = {&*It.begin(), false};
                            continue;
                        }
                    }
                }
            }
        }
    }

    for(auto [Symbol, T] : ConnectToBlock)
    {
        auto [Node, AtEnd] = T;
        if(gtirb::CodeBlock *CodeBlock = dyn_cast_or_null<gtirb::CodeBlock>(Node))
        {
            Symbol->setReferent(CodeBlock);
            Symbol->setAtEnd(AtEnd);
        }
        else if(gtirb::DataBlock *DataBlock = dyn_cast_or_null<gtirb::DataBlock>(Node))
        {
            Symbol->setReferent(DataBlock);
            Symbol->setAtEnd(AtEnd);
        }
    }

    // Connect remaining undefined external symbols to `ProxyBlocks'.
    auto *SymbolForwarding = Module.getAuxData<gtirb::schema::SymbolForwarding>();
    if(SymbolForwarding && SymbolInfo)
    {
        for(auto Forward : *SymbolForwarding)
        {
            gtirb::Node *Node = gtirb::Node::getByUUID(Context, std::get<1>(Forward));
            if(auto *Symbol = dyn_cast_or_null<gtirb::Symbol>(Node))
            {
                if(Symbol->hasReferent())
                {
                    continue;
                }
                if(auto It = SymbolInfo->find(Symbol->getUUID()); It != SymbolInfo->end())
                {
                    if(uint64_t SectionIndex = std::get<4>(It->second); SectionIndex == 0)
                    {
                        gtirb::ProxyBlock *ExternalBlock = Module.addProxyBlock(Context);
                        Symbol->setReferent(ExternalBlock);
                    }
                }
            }
        }
    }
}

void splitSymbols(gtirb::Context &Context, gtirb::Module &Module, souffle::SouffleProgram *Program)
{
    for(auto &T : *Program->getRelation("boundary_label"))
    {
        gtirb::Addr EA, Start, End;
        T >> EA >> Start >> End;

        if(auto It = Module.findDataBlocksOn(EA); !It.empty())
        {
            gtirb::DataBlock &Block = It.front();
            if(gtirb::ByteInterval *BI = Block.getByteInterval(); BI && BI->getAddress())
            {
                uint64_t Offset = EA - *(BI->getAddress());
                if(gtirb::SymbolicExpression *Expr = BI->getSymbolicExpression(Offset))
                {
                    if(auto *SAA = std::get_if<gtirb::SymAddrAddr>(Expr))
                    {
                        gtirb::Symbol *S = SAA->Sym1;
                        if(S && !S->getAtEnd() && S->getAddress() == End)
                        {
                            std::stringstream Stream;
                            Stream << "__end_" << std::hex << static_cast<uint64_t>(Start);
                            std::string Label = Stream.str();

                            gtirb::Symbol *NewSymbol = Module.addSymbol(Context, End, Label);
                            if(auto BlockIt = Module.findCodeBlocksOn(Start); !BlockIt.empty())
                            {
                                gtirb::CodeBlock &CodeBlock = BlockIt.front();
                                NewSymbol->setReferent(&CodeBlock);
                            }
                            NewSymbol->setAtEnd(true);
                            SAA->Sym1 = NewSymbol;
                        }
                    }
                }
            }
        }
    }
}

void buildFunctions(gtirb::Context &Context, gtirb::Module &Module, souffle::SouffleProgram *Prog)
{
    std::map<gtirb::UUID, std::set<gtirb::UUID>> FunctionEntries;
    std::map<gtirb::Addr, gtirb::UUID> FunctionEntry2Function;
    std::map<gtirb::UUID, gtirb::UUID> FunctionNames;
    boost::uuids::random_generator Generator;

    for(auto &T : *Prog->getRelation("function_inference.function_entry_name"))
    {
        gtirb::Addr FunctionEntry;
        std::string FunctionName;
        T >> FunctionEntry >> FunctionName;

        auto BlockRange = Module.findCodeBlocksAt(FunctionEntry);
        if(!BlockRange.empty())
        {
            const gtirb::UUID &EntryBlockUUID = BlockRange.begin()->getUUID();
            gtirb::UUID FunctionUUID = Generator();

            FunctionEntry2Function[FunctionEntry] = FunctionUUID;
            FunctionEntries[FunctionUUID].insert(EntryBlockUUID);

            gtirb::Symbol *FunctionNameSymbol = findFirstSymbol(Module, FunctionName);

            FunctionNames.insert({FunctionUUID, FunctionNameSymbol->getUUID()});
        }
    }

    std::map<gtirb::UUID, std::set<gtirb::UUID>> FunctionBlocks;
    for(auto &T : *Prog->getRelation("function_inference.in_function"))
    {
        gtirb::Addr BlockAddr, FunctionEntryAddr;
        T >> BlockAddr >> FunctionEntryAddr;
        auto BlockRange = Module.findCodeBlocksOn(BlockAddr);
        if(!BlockRange.empty())
        {
            gtirb::CodeBlock *Block = &*BlockRange.begin();
            gtirb::UUID FunctionEntryUUID = FunctionEntry2Function[FunctionEntryAddr];
            FunctionBlocks[FunctionEntryUUID].insert(Block->getUUID());
        }
    }

    Module.addAuxData<gtirb::schema::FunctionEntries>(std::move(FunctionEntries));
    Module.addAuxData<gtirb::schema::FunctionBlocks>(std::move(FunctionBlocks));
    Module.addAuxData<gtirb::schema::FunctionNames>(std::move(FunctionNames));
}

gtirb::EdgeType getEdgeType(const std::string &type)
{
    if(type == "branch")
        return gtirb::EdgeType::Branch;
    if(type == "call")
        return gtirb::EdgeType::Call;
    if(type == "return")
        return gtirb::EdgeType::Return;
    // TODO syscall and sysret
    return gtirb::EdgeType::Fallthrough;
}

void buildCFG(gtirb::Context &context, gtirb::Module &module, souffle::SouffleProgram *prog)
{
    auto &cfg = module.getIR()->getCFG();
    for(auto &output : *prog->getRelation("cfg_edge"))
    {
        gtirb::Addr srcAddr, destAddr;
        std::string conditional, indirect, type;
        output >> srcAddr >> destAddr >> conditional >> indirect >> type;

        // ddisasm guarantees that these blocks exist
        const gtirb::CodeBlock *src = &*module.findCodeBlocksOn(srcAddr).begin();
        const gtirb::CodeBlock *dest = &*module.findCodeBlocksOn(destAddr).begin();

        auto isConditional = conditional == "true" ? gtirb::ConditionalEdge::OnTrue
                                                   : gtirb::ConditionalEdge::OnFalse;
        auto isIndirect =
            indirect == "true" ? gtirb::DirectEdge::IsIndirect : gtirb::DirectEdge::IsDirect;
        gtirb::EdgeType edgeType = getEdgeType(type);

        auto E = addEdge(src, dest, cfg);
        cfg[*E] = std::make_tuple(isConditional, isIndirect, edgeType);
    }
    auto *topBlock = module.addProxyBlock(context);
    for(auto &output : *prog->getRelation("cfg_edge_to_top"))
    {
        gtirb::Addr srcAddr;
        std::string conditional, type;
        output >> srcAddr >> conditional >> type;
        const gtirb::CodeBlock *src = &*module.findCodeBlocksOn(srcAddr).begin();
        auto isConditional = conditional == "true" ? gtirb::ConditionalEdge::OnTrue
                                                   : gtirb::ConditionalEdge::OnFalse;
        gtirb::EdgeType edgeType = getEdgeType(type);
        auto E = addEdge(src, topBlock, cfg);
        cfg[*E] = std::make_tuple(isConditional, gtirb::DirectEdge::IsIndirect, edgeType);
    }
    for(auto &T : *prog->getRelation("cfg_edge_to_symbol"))
    {
        gtirb::Addr EA;
        std::string Name;
        std::string Type;
        T >> EA >> Name >> Type;

        const gtirb::CodeBlock *CodeBlock = &*module.findCodeBlocksOn(EA).begin();
        gtirb::Symbol &Symbol = *module.findSymbols(Name).begin();
        gtirb::ProxyBlock *ExternalBlock = Symbol.getReferent<gtirb::ProxyBlock>();
        if(!ExternalBlock)
        {
            // Create a ProxyBlock if the symbol does not already reference one.
            ExternalBlock = module.addProxyBlock(context);
            Symbol.setReferent(ExternalBlock);
        }

        gtirb::EdgeType EdgeType = getEdgeType(Type);
        auto E = addEdge(CodeBlock, ExternalBlock, cfg);
        cfg[*E] = {gtirb::ConditionalEdge::OnFalse, gtirb::DirectEdge::IsIndirect, EdgeType};
    }
}

// In general, it is expected that findOffsets returns a vector with zero or one items
// because blocks and data objects typically do not overlap.
std::vector<gtirb::Offset> findOffsets(gtirb::Module &module, gtirb::Addr ea)
{
    std::vector<gtirb::Offset> offsets;
    for(auto &block : module.findCodeBlocksOn(ea))
    {
        offsets.push_back(gtirb::Offset(block.getUUID(), ea - block.getAddress().value()));
    }
    for(auto &dataObject : module.findDataBlocksOn(ea))
    {
        offsets.push_back(
            gtirb::Offset(dataObject.getUUID(), ea - dataObject.getAddress().value()));
    }
    return offsets;
}

void updateComment(gtirb::Module &module, std::map<gtirb::Offset, std::string> &comments,
                   gtirb::Addr ea, std::string newComment)
{
    std::vector<gtirb::Offset> matchingOffsets = findOffsets(module, ea);
    for(gtirb::Offset &offset : matchingOffsets)
    {
        auto existing = comments.find(offset);
        if(existing != comments.end())
        {
            existing->second += ", ";
            existing->second += newComment;
        }
        else
            comments[offset] = newComment;
    }
}

void buildCfiDirectives(gtirb::Context &context, gtirb::Module &module,
                        souffle::SouffleProgram *prog)
{
    std::map<gtirb::Offset, std::vector<std::tuple<std::string, std::vector<int64_t>, gtirb::UUID>>>
        cfiDirectives;
    for(auto &output : *prog->getRelation("cfi_directive"))
    {
        gtirb::Addr blockAddr;
        std::string directive, reference;
        uint64_t disp, localIndex, nOperands;
        int64_t op1, op2;
        output >> blockAddr >> disp >> localIndex >> directive >> reference >> nOperands >> op1
            >> op2;
        std::vector<int64_t> operands;
        // cfi_escape directives have a sequence of bytes as operands (the raw bytes of the
        // dwarf instruction). The address 'reference' points to these bytes.
        if(directive == ".cfi_escape")
        {
            gtirb::Addr BytesLocation = gtirb::Addr(op1);
            if(const auto it = module.findByteIntervalsOn(BytesLocation); !it.empty())
            {
                if(const gtirb::ByteInterval &interval = *it.begin(); interval.getAddress())
                {
                    auto begin =
                        interval.bytes_begin<uint8_t>() + (BytesLocation - *interval.getAddress());
                    auto end = begin + nOperands;
                    for(uint8_t byte : boost::make_iterator_range(begin, end))
                    {
                        operands.push_back(static_cast<int64_t>(byte));
                    }
                }
            }
        }
        else
        {
            if(nOperands > 0)
                operands.push_back(op1);
            if(nOperands > 1)
                operands.push_back(op2);
        }

        auto blockRange = module.findCodeBlocksOn(blockAddr);
        if(blockRange.begin() != blockRange.end() && blockAddr == blockRange.begin()->getAddress())
        {
            gtirb::Offset offset(blockRange.begin()->getUUID(), disp);
            if(cfiDirectives[offset].size() < localIndex + 1)
                cfiDirectives[offset].resize(localIndex + 1);

            if(directive != ".cfi_escape" && reference != "")
            {
                // for normal directives (not cfi_escape) the reference points to a symbol.
                gtirb::Symbol *symbol = findFirstSymbol(module, reference);
                cfiDirectives[offset][localIndex] =
                    std::make_tuple(directive, operands, symbol->getUUID());
            }
            else
            {
                gtirb::UUID uuid;
                cfiDirectives[offset][localIndex] = std::make_tuple(directive, operands, uuid);
            }
        }
    }
    module.addAuxData<gtirb::schema::CfiDirectives>(std::move(cfiDirectives));
}

void buildPadding(gtirb::Module &Module, souffle::SouffleProgram *Prog)
{
    std::map<gtirb::Offset, uint64_t> Padding;
    for(auto &Output : *Prog->getRelation("padding"))
    {
        gtirb::Addr EA;
        uint64_t Size;
        Output >> EA >> Size;
        if(auto It = Module.findByteIntervalsOn(EA); !It.empty())
        {
            if(gtirb::ByteInterval &ByteInterval = *It.begin(); ByteInterval.getAddress())
            {
                uint64_t BlockOffset = EA - *ByteInterval.getAddress();
                gtirb::Offset Offset = gtirb::Offset(ByteInterval.getUUID(), BlockOffset);
                Padding[Offset] = Size;
            }
        }
    }
    Module.addAuxData<gtirb::schema::Padding>(std::move(Padding));
}

void buildComments(gtirb::Module &module, souffle::SouffleProgram *prog, bool selfDiagnose)
{
    std::map<gtirb::Offset, std::string> comments;
    for(auto &output : *prog->getRelation("data_access_pattern"))
    {
        gtirb::Addr ea;
        uint64_t size, from;
        int64_t multiplier;
        output >> ea >> size >> multiplier >> from;
        std::ostringstream newComment;
        newComment << "data_access(" << size << ", " << multiplier << ", " << std::hex << from
                   << std::dec << ")";
        updateComment(module, comments, ea, newComment.str());
    }

    for(auto &output : *prog->getRelation("preferred_data_access"))
    {
        gtirb::Addr ea;
        uint64_t data_access;
        output >> ea >> data_access;
        std::ostringstream newComment;
        newComment << "preferred_data_access(" << std::hex << data_access << std::dec << ")";
        updateComment(module, comments, ea, newComment.str());
    }

    for(auto &output : *prog->getRelation("best_value_reg"))
    {
        gtirb::Addr ea, eaOrigin;
        std::string reg, type;
        int64_t multiplier, offset;
        output >> ea >> reg >> eaOrigin >> multiplier >> offset >> type;
        std::ostringstream newComment;
        newComment << reg << "=X*" << multiplier << "+" << std::hex << offset << std::dec
                   << " type(" << type << ")";
        updateComment(module, comments, ea, newComment.str());
    }

    for(auto &output : *prog->getRelation("value_reg"))
    {
        gtirb::Addr ea, ea2;
        std::string reg, reg2;
        int64_t multiplier, offset;
        output >> ea >> reg >> ea2 >> reg2 >> multiplier >> offset;
        std::ostringstream newComment;
        newComment << reg << "=(" << reg2 << "," << std::hex << ea2 << std::dec << ")*"
                   << multiplier << "+" << std::hex << offset << std::dec;
        updateComment(module, comments, ea, newComment.str());
    }

    for(auto &output : *prog->getRelation("moved_label_class"))
    {
        gtirb::Addr ea;
        uint64_t opIndex;
        std::string type;

        output >> ea >> opIndex >> type;
        std::ostringstream newComment;
        newComment << " moved label-" << type;
        updateComment(module, comments, ea, newComment.str());
    }

    for(auto &output : *prog->getRelation("def_used"))
    {
        gtirb::Addr ea_use, ea_def;
        uint64_t index;
        std::string reg;
        output >> ea_def >> reg >> ea_use >> index;
        std::ostringstream newComment;
        newComment << "def(" << reg << ", " << std::hex << ea_def << std::dec << ")";
        updateComment(module, comments, ea_use, newComment.str());
    }
    for(auto &output : *prog->getRelation("missed_jump_table"))
    {
        gtirb::Addr ea;
        output >> ea;
        std::ostringstream newComment;
        newComment << "missed_jump_table";
        updateComment(module, comments, ea, newComment.str());
    }
    for(auto &output : *prog->getRelation("reg_has_base_image"))
    {
        gtirb::Addr ea;
        std::string reg;
        output >> ea >> reg;
        std::ostringstream newComment;
        newComment << "hasImageBase(" << reg << ")";
        updateComment(module, comments, ea, newComment.str());
    }
    if(selfDiagnose)
    {
        for(auto &output : *prog->getRelation("false_positive"))
        {
            gtirb::Addr ea;
            output >> ea;
            updateComment(module, comments, ea, "false positive");
        }
        for(auto &output : *prog->getRelation("false_negative"))
        {
            gtirb::Addr ea;
            output >> ea;
            updateComment(module, comments, ea, "false negative");
        }
        for(auto &output : *prog->getRelation("bad_symbol_constant"))
        {
            gtirb::Addr ea;
            uint64_t index;
            output >> ea >> index;
            std::ostringstream newComment;
            newComment << "bad_symbol_constant(" << index << ")";
            updateComment(module, comments, ea, newComment.str());
        }
    }
    module.addAuxData<gtirb::schema::Comments>(std::move(comments));
}

void updateEntryPoint(gtirb::Module &module, souffle::SouffleProgram *prog)
{
    for(auto &output : *prog->getRelation("entry_point"))
    {
        gtirb::Addr ea;
        output >> ea;

        if(const auto it = module.findCodeBlocksAt(ea); !it.empty())
        {
            module.setEntryPoint(&*it.begin());
        }
    }

    if(module.getFileFormat() != gtirb::FileFormat::RAW && !module.getEntryPoint())
    {
        std::cerr << "WARNING: Failed to set module entry point.\n";
    }
}

void disassembleModule(gtirb::Context &context, gtirb::Module &module,
                       souffle::SouffleProgram *prog, bool selfDiagnose)
{
    removeSectionSymbols(context, module);
    buildInferredSymbols(context, module, prog);
    buildSymbolForwarding(context, module, prog);
    buildCodeBlocks(context, module, prog);
    buildDataBlocks(context, module, prog);
    buildCodeSymbolicInformation(context, module, prog);
    buildCfiDirectives(context, module, prog);
    expandSymbolForwarding(context, module, prog);
    buildFunctions(context, module, prog);
    // This should be done after creating all the symbols.
    connectSymbolsToBlocks(context, module);
    splitSymbols(context, module, prog);
    // These functions should not create additional symbols.
    buildCFG(context, module, prog);
    buildPadding(module, prog);
    buildComments(module, prog, selfDiagnose);
    updateEntryPoint(module, prog);
    buildSymbolVersions(module);
}

void performSanityChecks(souffle::SouffleProgram *prog, bool selfDiagnose)
{
    bool error = false;
    if(selfDiagnose)
    {
        std::cout << "Perfoming self diagnose (this will only give the right results if the target "
                     "program contains all the relocation information)"
                  << std::endl;
        auto falsePositives = prog->getRelation("false_positive");
        if(falsePositives->size() > 0)
        {
            error = true;
            std::cerr << "False positives: " << falsePositives->size() << std::endl;
        }
        auto falseNegatives = prog->getRelation("false_negative");
        if(falseNegatives->size() > 0)
        {
            error = true;
            std::cerr << "False negatives: " << falseNegatives->size() << std::endl;
        }
        auto badSymbolCnt = prog->getRelation("bad_symbol_constant");
        if(badSymbolCnt->size() > 0)
        {
            error = true;
            std::cerr << "Bad symbol constants: " << badSymbolCnt->size() << std::endl;
        }
    }
    auto blockOverlap = prog->getRelation("block_still_overlap");
    if(blockOverlap->size() > 0)
    {
        error = true;
        std::cerr << "The conflicts between the following code blocks could not be resolved:"
                  << std::endl;
        for(auto &output : *blockOverlap)
        {
            uint64_t Block1, Block2;
            std::string BlockKind1, BlockKind2;
            output >> Block1 >> BlockKind1 >> Block2 >> BlockKind2;
            std::cerr << std::hex << Block1 << " (" << BlockKind1 << ") - " << Block2 << " ("
                      << BlockKind2 << ")" << std::dec << std::endl;
        }
    }
    if(error)
    {
        std::cerr << "Aborting" << std::endl;
        exit(1);
    }
    if(selfDiagnose && !error)
        std::cout << "Self diagnose completed: No errors found" << std::endl;
}
