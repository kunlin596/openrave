// -*- coding: utf-8 -*-
// Copyright (C) 2006-2013 Rosen Diankov <rosen.diankov@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
#include "openraveplugindefs.h"

// #define SHORTCUT_ONEDOF_DEBUG

class ShortcutLinearPlanner : public PlannerBase
{
public:
    ShortcutLinearPlanner(EnvironmentBasePtr penv, std::istream& sinput) : PlannerBase(penv)
    {
        __description = ":Interface Author: Rosen Diankov\n\npath optimizer using linear shortcuts.";
        _linearretimer = RaveCreatePlanner(GetEnv(), "LinearTrajectoryRetimer");
    }
    virtual ~ShortcutLinearPlanner() {
    }

    virtual bool InitPlan(RobotBasePtr pbase, PlannerParametersConstPtr params)
    {
        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        _parameters.reset(new TrajectoryTimingParameters());
        _parameters->copy(params);
        _probot = pbase;
        return _InitPlan();
    }

    virtual bool InitPlan(RobotBasePtr pbase, std::istream& isParameters)
    {
        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        _parameters.reset(new TrajectoryTimingParameters());
        isParameters >> *_parameters;
        _probot = pbase;
        return _InitPlan();
    }

    bool _InitPlan()
    {
        if( _parameters->_nMaxIterations <= 0 ) {
            _parameters->_nMaxIterations = 100;
        }
        if( _parameters->_fStepLength <= 0 ) {
            _parameters->_fStepLength = 0.04;
        }
        _linearretimer->InitPlan(RobotBasePtr(), _parameters);
        _puniformsampler = RaveCreateSpaceSampler(GetEnv(),"mt19937");
        if( !!_puniformsampler ) {
            _puniformsampler->SetSeed(_parameters->_nRandomGeneratorSeed);
        }

        _logginguniformsampler = RaveCreateSpaceSampler(GetEnv(),"mt19937");
        if( !!_logginguniformsampler ) {
            _logginguniformsampler->SetSeed(utils::GetMicroTime());
        }
        _fileindex = _logginguniformsampler->SampleSequenceOneUInt32()%1000;
        return !!_puniformsampler;
    }

    virtual PlannerParametersConstPtr GetParameters() const {
        return _parameters;
    }

    virtual PlannerStatus PlanPath(TrajectoryBasePtr ptraj)
    {
        BOOST_ASSERT(!!_parameters && !!ptraj );
        if( ptraj->GetNumWaypoints() < 2 ) {
            return PS_Failed;
        }

        RobotBase::RobotStateSaverPtr statesaver;
        if( !!_probot ) {
            statesaver.reset(new RobotBase::RobotStateSaver(_probot));
        }

        uint32_t basetime = utils::GetMilliTime();
        PlannerParametersConstPtr parameters = GetParameters();

//        if( IS_DEBUGLEVEL(Level_Verbose) ) {
//            // store the trajectory
//            uint32_t randnum;
//            if( !!_logginguniformsampler ) {
//                randnum = _logginguniformsampler->SampleSequenceOneUInt32();
//            }
//            else {
//                randnum = RaveRandomInt();
//            }
//            string filename = str(boost::format("%s/linearsmoother%d.parameters.xml")%RaveGetHomeDirectory()%(randnum%1000));
//            ofstream f(filename.c_str());
//            f << std::setprecision(std::numeric_limits<dReal>::digits10+1);     /// have to do this or otherwise precision gets lost
//            f << *_parameters;
//            RAVELOG_VERBOSE_FORMAT("saved linear parameters to %s", filename);
//            _DumpTrajectory(ptraj, Level_Verbose, 0);
//        }

        // subsample trajectory and add to list
        list< std::pair< vector<dReal>, dReal> > listpath;

#ifdef LINEAR_SMOOTHER_DEBUG
        TrajectoryBasePtr ptraj0 = RaveCreateTrajectory(GetEnv(), "");
        TrajectoryBasePtr ptraj1 = RaveCreateTrajectory(GetEnv(), "");
        TrajectoryBasePtr ptraj2 = RaveCreateTrajectory(GetEnv(), "");
        ptraj0->Init(parameters->_configurationspecification);
        ptraj1->Init(parameters->_configurationspecification);
        ptraj2->Init(parameters->_configurationspecification);
        ptraj0->Clone(ptraj, 0);
        _DumpTrajectory(ptraj, Level_Verbose, 0);
#endif

        _SubsampleTrajectory(ptraj,listpath);

#ifdef LINEAR_SMOOTHER_DEBUG
        ptraj->Init(parameters->_configurationspecification);
        FOREACH(it, listpath) {
            ptraj->Insert(ptraj->GetNumWaypoints(),it->first);
        }
        ptraj1->Clone(ptraj, 0);
        _DumpTrajectory(ptraj, Level_Verbose, 1);
#endif

        _OptimizePath(listpath);

#ifdef SHORTCUT_ONEDOF_DEBUG
        TrajectoryBasePtr ptrajbefore = RaveCreateTrajectory(GetEnv(), "");
        TrajectoryBasePtr ptrajafter = RaveCreateTrajectory(GetEnv(), "");
        ptrajbefore->Init(parameters->_configurationspecification);
        ptrajafter->Init(parameters->_configurationspecification);
        FOREACH(it, listpath) {
            ptrajbefore->Insert(ptrajbefore->GetNumWaypoints(), it->first);
        }
        _DumpTrajectory(ptrajbefore, Level_Debug, 1);
#endif

        _OptimizePathOneDOF(listpath);

#ifdef SHORTCUT_ONEDOF_DEBUG
        FOREACH(it, listpath) {
            ptrajafter->Insert(ptrajafter->GetNumWaypoints(), it->first);
        }
        _DumpTrajectory(ptrajafter, Level_Debug, 2);
#endif

        ptraj->Init(parameters->_configurationspecification);
        FOREACH(it, listpath) {
            ptraj->Insert(ptraj->GetNumWaypoints(),it->first);
        }

#ifdef LINEAR_SMOOTHER_DEBUG
        ptraj2->Clone(ptraj, 0);
        _DumpTrajectory(ptraj, Level_Verbose, 2);
#endif

        RAVELOG_DEBUG_FORMAT("env=%d, path optimizing - computation time=%fs\n", GetEnv()->GetId()%(0.001f*(float)(utils::GetMilliTime()-basetime)));
        if( parameters->_sPostProcessingPlanner.size() == 0 ) {
            // no other planner so at least retime
            PlannerStatus status = _linearretimer->PlanPath(ptraj);
            if( status != PS_HasSolution ) {
                return status;
            }
            return PS_HasSolution;
        }
        return _ProcessPostPlanners(RobotBasePtr(),ptraj);
    }

protected:
    void _OptimizePath(list< std::pair< vector<dReal>, dReal> >& listpath)
    {
        PlannerParametersConstPtr parameters = GetParameters();
        list< std::pair< vector<dReal>, dReal> >::iterator itstartnode, itendnode;
        if( !_filterreturn ) {
            _filterreturn.reset(new ConstraintFilterReturn());
        }

        int dof = parameters->GetDOF();
        int nrejected = 0;
        int iiter = parameters->_nMaxIterations;
        std::vector<dReal> vnewconfig0(dof), vnewconfig1(dof);
        while(iiter > 0  && nrejected < (int)listpath.size()+4 && listpath.size() > 2 ) {
            --iiter;

            // pick a random node on the listpath, and a random jump ahead
            uint32_t endIndex = 2+(_puniformsampler->SampleSequenceOneUInt32()%((uint32_t)listpath.size()-2));
            uint32_t startIndex = _puniformsampler->SampleSequenceOneUInt32()%(endIndex-1);

            itstartnode = listpath.begin();
            advance(itstartnode, startIndex);
            itendnode = itstartnode;
            dReal totaldistance = 0;
            for(uint32_t j = 0; j < endIndex-startIndex; ++j) {
                ++itendnode;
                totaldistance += itendnode->second;
            }
            nrejected++;

            dReal expectedtotaldistance = parameters->_distmetricfn(itstartnode->first, itendnode->first);
            if( expectedtotaldistance > totaldistance-0.1*parameters->_fStepLength ) {
                // expected total distance is not that great
                continue;
            }

            // check if the nodes can be connected by a straight line
            _filterreturn->Clear();
            if (parameters->CheckPathAllConstraints(itstartnode->first, itendnode->first, std::vector<dReal>(), std::vector<dReal>(), 0, IT_Open, 0xffff|CFO_FillCheckedConfiguration, _filterreturn) != 0 ) {
                if( nrejected++ > (int)listpath.size()+8 ) {
                    break;
                }
                continue;
            }
            if(_filterreturn->_configurations.size() == 0 ) {
                continue;
            }
            OPENRAVE_ASSERT_OP(_filterreturn->_configurations.size()%dof, ==, 0);
            // check how long the new path is
            _vtempdists.resize(_filterreturn->_configurations.size()/dof+1);
            std::vector<dReal>::iterator itdist = _vtempdists.begin();
            std::vector<dReal>::iterator itnewconfig = _filterreturn->_configurations.begin();
            std::copy(itnewconfig, itnewconfig+dof, vnewconfig0.begin());
            dReal newtotaldistance = parameters->_distmetricfn(itstartnode->first, vnewconfig0);
            *itdist++ = newtotaldistance;
            itnewconfig += dof;
            while(itnewconfig != _filterreturn->_configurations.end() ) {
                std::copy(itnewconfig, itnewconfig+dof, vnewconfig1.begin());
                *itdist = parameters->_distmetricfn(vnewconfig0, vnewconfig1);
                newtotaldistance += *itdist;
                ++itdist;
                vnewconfig0.swap(vnewconfig1);
                itnewconfig += dof;
            }
            *itdist = parameters->_distmetricfn(vnewconfig0, itendnode->first);
            newtotaldistance += *itdist;
            ++itdist;
            BOOST_ASSERT(itdist==_vtempdists.end());

            if( newtotaldistance > totaldistance-0.1*parameters->_fStepLength ) {
                // new path is not that good, so reject
                nrejected++;
                continue;
            }

            // finally add
            itdist = _vtempdists.begin();
            ++itstartnode;
            itnewconfig = _filterreturn->_configurations.begin();
            while(itnewconfig != _filterreturn->_configurations.end()) {
                std::copy(itnewconfig, itnewconfig+dof, vnewconfig1.begin());
                listpath.insert(itstartnode, make_pair(vnewconfig1, *itdist++));
                itnewconfig += dof;
            }
            itendnode->second = *itdist++;
            BOOST_ASSERT(itdist==_vtempdists.end());

            // splice out in-between nodes in path
            listpath.erase(itstartnode, itendnode);
            nrejected = 0;

            if( listpath.size() <= 2 ) {
                break;
            }

            if( IS_DEBUGLEVEL(Level_Verbose) ) {
                dReal newdistance = 0;
                FOREACH(ittempnode, listpath) {
                    newdistance += ittempnode->second;
                }
                RAVELOG_VERBOSE_FORMAT("shortcut iter=%d, path=%d, dist=%f", (parameters->_nMaxIterations-iiter)%listpath.size()%newdistance);
            }
        }
    }

    // Experimental function: shortcut only the last DOF (J6). Although some non-default
    // distmetric/neighstatefn/diffstatefn might be used here, we still compute the distance of the
    // shortcut dof simply by taking the difference between two configs.
    void _OptimizePathOneDOF(list< std::pair< vector<dReal>, dReal> >& listpath)
    {
        int numshortcuts = 0;
        PlannerParametersConstPtr parameters = GetParameters();
        PlannerProgress progress;
        int ndof = parameters->GetDOF();
        int nrejected = 0;
        std::list< std::pair< std::vector<dReal>, dReal > >::iterator itstartnode, itendnode, itnode;
        std::vector< std::pair< std::vector<dReal>, dReal > > vpathvalues; // for keeping the shortcut segment
        for( int iiter = 0; iiter < parameters->_nMaxIterations; ++iiter ) {
            if( listpath.size() <= 2 ) {
                return;
            }
            // Sample a DOF to shortcut. Give the last DOF twice as much chance.
            uint32_t idof = _puniformsampler->SampleSequenceOneUInt32()%(ndof + 1);
            if( idof > ndof - 1 ) {
                idof = ndof - 1;
            }

            // Pick a pair of nodes in listpath.
            uint32_t endIndex = 2 + (_puniformsampler->SampleSequenceOneUInt32()%((uint32_t)listpath.size() - 2));
            uint32_t startIndex = _puniformsampler->SampleSequenceOneUInt32()%(endIndex - 1);
            if( endIndex == startIndex + 1 ) {
                continue;
            }

            itstartnode = listpath.begin();
            std::advance(itstartnode, startIndex);
            itendnode = itstartnode;
            dReal totalDOFDistance = 0; // distance traveled by the dof idof
            dReal totalDistance = 0;
            dReal curDOFValue = itstartnode->first.at(idof);
            for( uint32_t j = 0; j < endIndex - startIndex; ++j ) {
                ++itendnode;
                totalDOFDistance += RaveFabs(itendnode->first.at(idof) - curDOFValue);
                totalDistance += itendnode->second;
                curDOFValue = itendnode->first.at(idof);
            }
            nrejected++;

            dReal expectedDOFDistance = itendnode->first.at(idof) - itstartnode->first.at(idof);
            // RAVELOG_DEBUG_FORMAT("env=%d, prevdofdist=%.15e; newdofdist=%.15e; diff=%.15e", GetEnv()->GetId()%totalDOFDistance%RaveFabs(expectedDOFDistance)%(totalDOFDistance - RaveFabs(expectedDOFDistance)));
            if( RaveFabs(expectedDOFDistance) > totalDOFDistance - 0.1*parameters->_vConfigResolution.at(idof) ) {
                // Even after a successful shortcut, the resulting path wouldn't be that much shorter. So skipping.
                continue;
            }

            progress._iteration = iiter;
            if( _CallCallbacks(progress) == PA_Interrupt ) {
                return;
            }

            bool bsuccess = true;
            vpathvalues.resize(endIndex - startIndex + 1);
            int pathindex = 0;
            dReal fdelta = expectedDOFDistance / totalDistance;
            dReal fcurdist = 0;
            dReal fstartdofvalue = itstartnode->first.at(idof);
            itnode = itstartnode;
            vpathvalues.at(0).first = itnode->first;
            vpathvalues.at(0).second = itnode->second;
            // RAVELOG_DEBUG_FORMAT("env=%d, iter=%d/%d, totalDistance=%.15e; fdelta=%.15e", GetEnv()->GetId()%iiter%parameters->_nMaxIterations%totalDistance%fdelta);
            // std::stringstream ss; ss << "dof values=[";
            do {
                ++itnode;
                ++pathindex;
                fcurdist += itnode->second;
                vpathvalues.at(pathindex).first = itnode->first;
                // compute an appropriate value for idof by curdof = startdofvalue + (curdist/alldist)*expectedDOFDistance
                vpathvalues.at(pathindex).first.at(idof) = fstartdofvalue + fcurdist*fdelta;
                dReal fdist = parameters->_distmetricfn(vpathvalues.at(pathindex).first, vpathvalues.at(pathindex - 1).first);
                vpathvalues.at(pathindex).second = fdist;
                // ss << vpathvalues.at(pathindex).first.at(idof) << ", ";
            } while( itnode != itendnode );
            // ss << "];";
            // RAVELOG_DEBUG_FORMAT("env=%d, iter=%d/%d, finished computing vpathvalues, size=%d; %s", GetEnv()->GetId()%iiter%parameters->_nMaxIterations%vpathvalues.size()%ss.str());

            for( size_t ipath = 0; ipath < vpathvalues.size(); ++ipath ) {
                if( !(parameters->CheckPathAllConstraints(vpathvalues.at(ipath).first, vpathvalues.at(ipath).first, std::vector<dReal>(), std::vector<dReal>(), 0, IT_OpenStart, 0xffff) == 0) ) {
                    bsuccess = false;
                    break;
                }
            }

            progress._iteration = iiter;
            if( _CallCallbacks(progress) == PA_Interrupt ) {
                return;
            }

            if( !bsuccess ) {
                continue;
            }

            ++numshortcuts;
            // Replace the original segment with the new one
            itnode = itstartnode;
            pathindex = 1;
            do {
                *itnode = vpathvalues.at(pathindex);
                ++pathindex;
                ++itnode;
            } while( itnode != itendnode );
            nrejected = 0;
        }
        // RAVELOG_DEBUG_FORMAT("env=%d, successful shortcuts=%d", GetEnv()->GetId()%numshortcuts);
        return;
    }

    void _SubsampleTrajectory(TrajectoryBasePtr ptraj, list< std::pair< vector<dReal>, dReal> >& listpath) const
    {
        PlannerParametersConstPtr parameters = GetParameters();
        vector<dReal> q0(parameters->GetDOF()), q1(parameters->GetDOF()), dq(parameters->GetDOF()), qcur(parameters->GetDOF()), dq2;
        vector<dReal> vtrajdata;
        ptraj->GetWaypoints(0,ptraj->GetNumWaypoints(),vtrajdata,parameters->_configurationspecification);

        std::copy(vtrajdata.begin(),vtrajdata.begin()+parameters->GetDOF(),q0.begin());
        listpath.push_back(make_pair(q0,dReal(0)));
        qcur = q0;

        for(size_t ipoint = 1; ipoint < ptraj->GetNumWaypoints(); ++ipoint) {
            std::copy(vtrajdata.begin()+(ipoint)*parameters->GetDOF(),vtrajdata.begin()+(ipoint+1)*parameters->GetDOF(),q1.begin());
            dq = q1;
            parameters->_diffstatefn(dq,q0);
            int i, numSteps = 1;
            vector<dReal>::const_iterator itres = parameters->_vConfigResolution.begin();
            for (i = 0; i < parameters->GetDOF(); i++,itres++) {
                int steps;
                if( *itres != 0 ) {
                    steps = (int)(RaveFabs(dq[i]) / *itres);
                }
                else {
                    steps = (int)(RaveFabs(dq[i]) * 100);
                }
                if (steps > numSteps) {
                    numSteps = steps;
                }
            }
            dReal fisteps = dReal(1.0f)/numSteps;
            FOREACH(it,dq) {
                *it *= fisteps;
            }
            int mult = 1;
            for (int f = 1; f < numSteps; f++) {
                int neighstatus = NSS_Failed;
                if( mult > 1 ) {
                    dq2 = dq;
                    FOREACHC(it, dq2) {
                        *it *= mult;
                    }
                    neighstatus = parameters->_neighstatefn(qcur,dq2,NSO_OnlyHardConstraints);
                }
                else {
                    neighstatus = parameters->_neighstatefn(qcur,dq,NSO_OnlyHardConstraints);
                }
                if( neighstatus == NSS_SuccessfulWithDeviation ) {
                    RAVELOG_WARN_FORMAT("env=%d, neighstatefn returned different configuration than qcur, stop subsampling segment (%d, %d) at step %d/%d, numwaypoints=%d", GetEnv()->GetId()%(ipoint-1)%ipoint%f%numSteps%ptraj->GetNumWaypoints());
                    qcur = listpath.back().first; // restore qcur
                    break;
                }
                else if( neighstatus == NSS_Failed ) {
                    RAVELOG_DEBUG_FORMAT("env=%d, neighstatefn failed mult=%d, perhaps non-linear constraints are used?", GetEnv()->GetId()%mult);
                    mult++;
                    continue;
                }
                dReal dist = parameters->_distmetricfn(listpath.back().first,qcur);
                listpath.push_back(make_pair(qcur, dist));
                mult = 1;
            }
            // always add the last point
            dReal dist = parameters->_distmetricfn(listpath.back().first,q1);
            listpath.push_back(make_pair(q1, dist));
            qcur = q1;
            q0.swap(q1);
        }

        std::copy(vtrajdata.end()-parameters->GetDOF(),vtrajdata.end(),q0.begin());
        dReal dist = parameters->_distmetricfn(listpath.back().first,q0);
        listpath.push_back(make_pair(q0,dist));
    }

    std::string _DumpTrajectory(TrajectoryBasePtr traj, DebugLevel level, int option)
    {
        if( IS_DEBUGLEVEL(level) ) {
            std::string filename = _DumpTrajectory(traj, option);
            RavePrintfA(str(boost::format("env=%d, wrote linearshortcutadvanced trajectory to %s")%GetEnv()->GetId()%filename), level);
            return filename;
        }
        return std::string();
    }

    std::string _DumpTrajectory(TrajectoryBasePtr traj, int option)
    {
        string filename;
        if (option == 0) {
            filename = str(boost::format("%s/linearshortcutadvanced%d.initial.xml")%RaveGetHomeDirectory()%(_fileindex));
        }
        else if (option == 1) {
            filename = str(boost::format("%s/linearshortcutadvanced%d.beforeshortcut.xml")%RaveGetHomeDirectory()%(_fileindex));
        }
        else if (option == 2) {
            filename = str(boost::format("%s/linearshortcutadvanced%d.aftershortcut.xml")%RaveGetHomeDirectory()%(_fileindex));
        }
        else {
            filename = str(boost::format("%s/linearshortcutadvanced%d.traj.xml")%RaveGetHomeDirectory()%(_fileindex));
        }
        ofstream f(filename.c_str());
        f << std::setprecision(std::numeric_limits<dReal>::digits10+1);     /// have to do this or otherwise precision gets lost
        traj->serialize(f);
        return filename;
    }

    TrajectoryTimingParametersPtr _parameters;
    SpaceSamplerBasePtr _puniformsampler, _logginguniformsampler;
    uint32_t _fileindex;

    RobotBasePtr _probot;
    PlannerBasePtr _linearretimer;
    ConstraintFilterReturnPtr _filterreturn;
    std::vector<dReal> _vtempdists;
};

PlannerBasePtr CreateShortcutLinearPlanner(EnvironmentBasePtr penv, std::istream& sinput) {
    return PlannerBasePtr(new ShortcutLinearPlanner(penv, sinput));
}
