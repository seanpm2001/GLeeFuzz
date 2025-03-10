#include <iostream>
#include <fstream>
#include <string>
#include <tuple>

#include <nlohmann/json.hpp>

#include "SVF-FE/LLVMUtil.h"
#include "SVF-FE/CPPUtil.h"

#include "Graphs/SVFG.h"
#include "WPA/Andersen.h"
#include "SVF-FE/SVFIRBuilder.h"
#include "Util/BasicTypes.h"
#include "Util/SVFUtil.h"
#include "Util/Options.h"

#if LLVM_VERSION_MAJOR > 10
#include <llvm/IR/AbstractCallSite.h>
#endif

#include "svf_utils.hpp"
#include "llvm_utils.hpp"

using namespace llvm;
using namespace std;
using namespace SVF;

using json = nlohmann::json;

cl::OptionCategory
cat("err_msg_analysis Options",
    "These control the inputs to err_msg_analysis tool.");

static cl::opt<string>
ir("ir", cl::desc("Specify WebGL API IR file"),
   cl::value_desc("IR file"), cl::Required,
   cl::cat(cat));

static cl::opt<string>
am_file("am",
        cl::desc("Specify the json file containing api func "
                 "mapping result (generated by api_func_parse)"),
        cl::value_desc("api_func_map"),
        cl::Required,
        cl::cat(cat));

static cl::opt<int>
api_id("api_id", cl::desc("the api to analysis (with id)"),
       cl::value_desc("the id of the api"),
       cl::init(-1),
       cl::cat(cat));

static cl::opt<string> eef("eef", cl::desc("the name of error message emitting function"),
                        cl::value_desc("the manged function name"),
                        cl::init("_ZN5blink25WebGLRenderingContextBase17"
                                 "SynthesizeGLErrorEjPKcS2_NS0_24ConsoleDisplayPreferenceE"),
                        cl::cat(cat));

#define GL_INVALID_OPERATION 0x0502

class CallToFuncVisitor : public InstVisitor<CallToFuncVisitor> {
    private:
        string &_fname;
        set<const Instruction *> _res;

  public:
        CallToFuncVisitor(string &fname)
            : _fname(fname) {}

        void visitCallInst(CallInst &callInst) {

#ifdef _DBG
            llvm::outs() << "Handling callInst: \n\t"<< callInst << "\n";
            llvm::outs() << "\t" << SVFUtil::getSourceLoc(&callInst) << "\n";
#endif

#if LLVM_VERSION_MAJOR < 11
            // llvm::outs() << callInst << "\n";
            ImmutableCallSite cs(&callInst);
            if (const auto *f = cs.getCalledFunction()) {
                if (f->getName().str() == _fname) {
                    _res.insert(cs.getInstruction());
                }
            }
#else
            AbstractCallSite cs(&callInst.getCalledOperandUse());
            if (cs.isDirectCall()) {
                if (const auto *f = cs.getCalledFunction()) {
                    if (f->getName().str() == _fname) {
                        _res.insert(cs.getInstruction());
                    }
                }
            } else {

#ifdef _DBG
                llvm::outs() << "\t not a direct call\n";
#endif

            }
#endif
        }

        set<const Instruction *> &getRes() {
            return _res;
        }
};



static void collectErrMsg(SVFModule *svfModule,
                          ICFG *icfg,
                          json &api_mapping) {

    CallToFuncVisitor cfVisitor(eef.getValue());
    traverseFunctionICFG(icfg,  svfModule, api_mapping["llvm_fname"], cfVisitor);

    llvm::outs() << "id: " << api_mapping["id"].get<int>() << ", name: " << api_mapping["name"].get<string>() << " {\n";

    llvm::outs() << "Results as follows: \n";
    auto &res = cfVisitor.getRes();
    for (auto v : res) {
        llvm::outs() << "---------------------------\n";
        llvm::outs() << *v << "\n";
        llvm::outs() << SVFUtil::getSourceLoc(v) << "\n";
        if (auto *ci = llvm::dyn_cast<CallInst>(v)) {
            tuple<string, int> einfo = extract_log_message_from_callinst(ci);
            llvm::outs() << "ec: " << get<1>(einfo) << "\n";
            llvm::outs() << "message: " << get<0>(einfo) << "\n";
        } else {
            llvm::outs() << "XXXXX Not a CallInst\n";
        }

        llvm::outs() << "===========================\n";
    }

    llvm::outs() << "}\n";
}

int main(int argc, char **argv) {
    cl::ParseCommandLineOptions(argc, argv, "Analyzing error messages of APIs\n");

    SVFModule* svfModule = LLVMModuleSet::getLLVMModuleSet()->buildSVFModule({ir});
    svfModule->buildSymbolTableInfo();

    SVFIRBuilder builder;
    SVFIR* pag = builder.build(svfModule);

    Andersen* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    PTACallGraph* callgraph = ander->getPTACallGraph();
    VFG* vfg = new VFG(callgraph);
    ICFG* icfg = pag->getICFG();

    SVFGBuilder svfBuilder(true);
    SVFG* svfg = svfBuilder.buildFullSVFG(ander);

    ifstream ifs(am_file.getValue());
    json am;
    ifs >> am;
    json &mapping = am["mappings"];
    int sid = api_id.getValue();
    if (sid >= 0 && sid < mapping.size()) {
        json &api_mapping = mapping[api_id.getValue()];
        collectErrMsg(svfModule, icfg, api_mapping);
    } else {
        for (int i = 0; i < mapping.size(); i ++) {
            collectErrMsg(svfModule, icfg, mapping[i]);
        }
    }

    delete svfg;
    delete vfg;
    AndersenWaveDiff::releaseAndersenWaveDiff();
    SVFIR::releaseSVFIR();

    return 0;
}
