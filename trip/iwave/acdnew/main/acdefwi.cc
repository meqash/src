#include "acd_defn.hh"
#include "grid.h"
#include "gridpp.hh"
#include "gridops.hh"
#include "iwop.hh"
#include "functions.hh"
#include "op.hh"
#include "ls.hh"
#include "blockop.hh"
#include "cgnealg.hh"
#include "LBFGSBT.hh"
#include "acdefwi_selfdoc.h"
#include <par.h>
#include "segyops.hh"

//#define GTEST_VERBOSE
IOKEY IWaveInfo::iwave_iokeys[]
= {
  {"csqext",    0, true,  true },
  {"data",   1, false, true },
  {"source", 1, true,  false},
  {"",       0, false, false}
};

using RVL::parse;
using RVL::RVLException;
using RVL::Vector;
using RVL::Components;
using RVL::Operator;
using RVL::OperatorEvaluation;
using RVL::LinearOp;
using RVL::LinearOpFO;
using RVL::OpComp;
using RVL::RegLeastSquaresFcnlGN;
using RVL::SymmetricBilinearOp;
using RVL::AssignFilename;
using RVL::AssignParams;
using RVL::RVLRandomize;
using RVL::ScaleOpFwd;
using RVL::TensorOp;
using RVLUmin::CGNEPolicy;
using RVLUmin::CGNEPolicyData;
using RVLUmin::LBFGSBT;
using RVLUmin::CGNEAlg;
using TSOpt::IWaveEnvironment;
using TSOpt::IWaveTree;
using TSOpt::IWaveSampler;
using TSOpt::IWaveSim;
using TSOpt::TASK_RELN;
using TSOpt::IOTask;
using TSOpt::IWaveOp;
using TSOpt::SEGYTaperMute;
using TSOpt::GridMaskOp;
#ifdef IWAVE_USE_MPI
using TSOpt::MPIGridSpace;
using TSOpt::MPISEGYSpace;
#else
using TSOpt::GridSpace;
using TSOpt::SEGYSpace;
#endif
using TSOpt::GridExtendOp;
using TSOpt::GridDerivOp;

int xargc;
char **xargv;

/** this version requires that two model files be present:
    csqext - extended version of csq, with [d=spatial dimn]
    gdim=d+1
    dim=d
    n[d+1]=number of shot records, 
    d[d+1]=1
    o[d+1]=0
    id[d+1]=d+1
    csq - physical space version of csq, with dim=gdim=d
    Uses these keywords for geometry only - data content irrelevant

    [intended near-term update: require only physical space material
    params, generate extended version internally, so automatic 
    compatibility with data]

*/
int main(int argc, char ** argv) {

  try {

#ifdef IWAVE_USE_MPI
    int ts=0;
    MPI_Init_thread(&argc,&argv,MPI_THREAD_FUNNELED,&ts);    
#endif

    PARARRAY * pars = NULL;
    FILE * stream = NULL;
    IWaveEnvironment(argc, argv, 0, &pars, &stream);

    if (retrieveGlobalRank()==0 && argc<2) {
      pagedoc();
      exit(0);
    }

#ifdef IWAVE_USE_MPI
    if (retrieveGroupID() == MPI_UNDEFINED) {
      fprintf(stream,"NOTE: finalize MPI, cleanup, exit\n");
      fflush(stream);
    }
    else {
#endif

      /* the Op */
      IWaveOp iwop(*pars,stream);
        
      SEGYTaperMute tnm(valparse<float>(*pars,"mute_slope",0.0f),
                        valparse<float>(*pars,"mute_zotime",0.0f),
                        valparse<float>(*pars,"mute_width",0.0f),0,
                        valparse<float>(*pars,"min",0.0f),
                        valparse<float>(*pars,"max",numeric_limits<float>::max()
),
                        valparse<float>(*pars,"taper_width",0.0f),
                        valparse<int>(*pars,"taper_type",0),
                        valparse<float>(*pars,"time_width",0.0f),
                        valparse<float>(*pars,"sx_min",0.0f),
                        valparse<float>(*pars,"sx_max",numeric_limits<float>::max()),
                        valparse<float>(*pars,"sx_width",0.0f));
        
      LinearOpFO<float> tnmop(iwop.getRange(),iwop.getRange(),tnm,tnm);
      OpComp<float> op(iwop,tnmop);
   
/* generate physical model space - a priori distinct from extended space
 * without even the same grid */
#ifdef IWAVE_USE_MPI
            MPIGridSpace csqsp(valparse<std::string>(*pars,"csq"),"notype",true);
#else
            GridSpace csqsp(valparse<std::string>(*pars,"csq"),"notype",true);
#endif
 
      // vel-squared model!
      Vector<ireal> m0(op.getDomain());
      Vector<ireal> m(op.getDomain());

      AssignFilename mf0n(valparse<std::string>(*pars,"init_csqext"));
      Components<ireal> cm0(m0);
      cm0[0].eval(mf0n);

      AssignFilename mfn(valparse<std::string>(*pars,"final_csqext"));
      Components<ireal> cm(m);
      cm[0].eval(mfn);
      m.copy(m0);
    
      // muted data
      Vector<ireal> mdd(op.getRange());
      std::string mddnm = valparse<std::string>(*pars,"datamut","");
      if (mddnm.size()>0) {
        AssignFilename mddfn(mddnm);
        mdd.eval(mddfn);
      }
      { // read data - only needed to define muted data
        Vector<ireal> dd(op.getRange());
        AssignFilename ddfn(valparse<std::string>(*pars,"data"));
        Components<ireal> cdd(dd);
        cdd[0].eval(ddfn);
        tnmop.applyOp(dd,mdd);
      }

      /* output stream */
      std::stringstream res;
      res<<scientific;
    
      // choice of GridDerivOp for semblance op is placeholder - extended axis is dim-th, with id=dim+1
      // note default weight of zero!!!
      // Note added 10.03.14: getGrid is not usably implemented for MPIGridSpace at this time, so 
      // must fish the derivative index out by hand and bcast
      // THIS SHOULD ALL BE FIXED! (1) getGrid properly implemented in parallel, (2) proper
      // defn of DSOp to include internal extd axis case (space/time shift)
      int dsdir = INT_MAX;
      if (retrieveGlobalRank()==0) dsdir=csqsp.getGrid().dim;
#ifdef IWAVE_USE_MPI
      if (MPI_Bcast(&dsdir,1,MPI_INT,0,retrieveGlobalComm())) {
	RVLException e;
	e<<"Error: acdiva, rank="<<retrieveGlobalRank()<<"\n";
	e<<"  failed to bcast dsdir\n";
	throw e;
      }
#endif
    // assign window widths - default = 0;
    RPNT swind,ewind,width;
    RASN(swind,RPNT_0);
    RASN(ewind,RPNT_0);
    swind[0]=valparse<float>(*pars,"sww1",0.0f);
    swind[1]=valparse<float>(*pars,"sww2",0.0f);
    swind[2]=valparse<float>(*pars,"sww3",0.0f);
    ewind[0]=valparse<float>(*pars,"eww1",0.0f);
    ewind[1]=valparse<float>(*pars,"eww2",0.0f);
    ewind[2]=valparse<float>(*pars,"eww3",0.0f);
     width[0]=valparse<float>(*pars,"width0",0.0f);
     width[1]=valparse<float>(*pars,"width1",0.0f);
     width[2]=valparse<float>(*pars,"width2",0.0f);


     GridDerivOp dsop(op.getDomain(),dsdir,1.0f);//valparse<float>(*pars,"DSWeight",0.0f));
     // need to read in model space for bg input to GridWindowOp
     Vector<ireal> m_in(op.getDomain());
     AssignFilename minfn(valparse<std::string>(*pars,"csqext"));
     Components<ireal> cmin(m_in);
     cmin[0].eval(minfn);
     GridMaskOp mop(op.getDomain(),m_in,swind,ewind,width);
     OperatorEvaluation<float> mopeval(mop,m_in);
     //     LinearOp<float> const & lmop=mopeval.getDeriv();
     OpComp<float> cop(mop,op);
     OpComp<float> cdsop(mop,dsop);

      // choice of preop is placeholder
      // ScaleOpFwd<float> preop(cop.getDomain(),1.0f);
      Vector<ireal> zm(op.getDomain());
      zm.zero();
      RegLeastSquaresFcnlGN<float> f(cop, dsop, mdd, zm, valparse<float>(*pars,"DSWeight",0.0f));

      // lower, upper bds for csq
      Vector<float> lb(f.getDomain());
      Vector<float> ub(f.getDomain());
      float lbcsq=valparse<float>(*pars,"cmin");
      lbcsq=lbcsq*lbcsq;
      RVLAssignConst<float> asl(lbcsq);
      lb.eval(asl);
      float ubcsq=valparse<float>(*pars,"cmax");
      ubcsq=ubcsq*ubcsq;
      RVLAssignConst<float> asu(ubcsq);
      ub.eval(asu);
      RVLMin<float> mn;
#ifdef IWAVE_USE_MPI
      MPISerialFunctionObjectRedn<float,float> mpimn(mn);
      ULBoundsTest<float> ultest(lb,ub,mpimn);
#else
      ULBoundsTest<float> ultest(lb,ub,mn);
#endif

      FunctionalBd<float> fbd(f,ultest);

      LBFGSBT<float> alg(fbd,m,
			 valparse<float>(*pars,"InvHessianScale",1.0f),
			 valparse<int>(*pars,"MaxInvHessianUpdates",5),
			 valparse<int>(*pars,"MaxLineSrchSteps",10),
			 valparse<bool>(*pars,"VerboseDisplay",true), 
			 valparse<float>(*pars,"FirstStepLength",1.0f), 
			 valparse<float>(*pars,"GAStepAcceptThresh",0.1f),
			 valparse<float>(*pars,"GAStepDoubleThresh",0.9f), 
			 valparse<float>(*pars,"LSBackTrackFac",0.5f), 
			 valparse<float>(*pars,"LSDoubleFac",1.8f),
			 valparse<float>(*pars,"MaxFracDistToBdry",1.0), 
			 valparse<float>(*pars,"LSMinStepFrac",1.e-06),
			 valparse<int>(*pars,"MaxLBFGSIter",3), 
			 valparse<float>(*pars,"AbsGradThresh",0.0), 
			 valparse<float>(*pars,"RelGradThresh",1.e-2), 
			 res);
      if (valparse<int>(*pars,"MaxLBFGSIter",3) <= 0) {
	float val = alg.getFunctionalEvaluation().getValue();
	res<<"=========================== LINFITLS ==========================\n";
	res<<"value of IVA functional = "<<val<<"\n";    
	res<<"=========================== LINFITLS ==========================\n";
      }
      else {
	alg.run();
      }

      std::string dataest = valparse<std::string>(*pars,"dataest","");
      std::string datares = valparse<std::string>(*pars,"datares","");
      std::string normalres = valparse<std::string>(*pars,"normalres","");
      if (dataest.size()>0) {
          OperatorEvaluation<float> opeval(op,m);
          Vector<float> est(op.getRange());
          AssignFilename estfn(dataest);
          est.eval(estfn);
          est.copy(opeval.getValue());
          if (datares.size()>0) {
              Vector<float> res(op.getRange());
              AssignFilename resfn(datares);
              res.eval(resfn);
              res.copy(est);
              res.linComb(-1.0f,mdd);
          }
      }

      if (retrieveRank() == 0) {
	std::string outfile = valparse<std::string>(*pars,"outfile","");
	if (outfile.size()>0) {
	  ofstream outf(outfile.c_str());
	  outf<<res.str();
	  outf.close();
	}
	else {
	  cout<<res.str();
	}
      }
    
#ifdef IWAVE_USE_MPI
    }
    MPI_Finalize();
#endif
  }
  catch (RVLException & e) {
    e.write(cerr);
#ifdef IWAVE_USE_MPI
    MPI_Abort(MPI_COMM_WORLD,0);
#endif
    exit(1);
  }
  
}
