/** @file
    @brief Implementation

    @date 2015

    @author
    Sensics, Inc.
    <http://sensics.com/osvr>
*/

// Copyright 2015 Sensics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Internal Includes
#include "VideoIMUFusion.h"
#include <osvr/AnalysisPluginKit/AnalysisPluginKitC.h>
#include <osvr/ClientKit/InterfaceCallbackC.h>
#include <osvr/ClientKit/InterfaceStateC.h>
#include <osvr/Util/EigenFilters.h>
#include <osvr/Util/EigenInterop.h>

#include "FlexibleKalmanFilter.h"
#include "PoseDampedConstantVelocity.h"
#include "AbsoluteOrientationMeasurement.h"
#include "AbsolutePoseMeasurement.h"

// Generated JSON header file
#include "org_osvr_filter_videoimufusion_json.h"

// Library/third-party includes
#include <boost/assert.hpp>

// Standard includes
#include <iostream>

using ProcessModel = osvr::kalman::PoseDampedConstantVelocityProcessModel;
using FilterState = ProcessModel::State;
using AbsoluteOrientationMeasurement =
    osvr::kalman::AbsoluteOrientationMeasurement<FilterState>;
using AbsolutePoseMeasurement =
    osvr::kalman::AbsolutePoseMeasurement<FilterState>;
using Filter = osvr::kalman::FlexibleKalmanFilter<ProcessModel>;

namespace detail {
template <typename ReportType>
void callbackCaller(void *userdata, const OSVR_TimeValue *timestamp,
                    const ReportType *report) {
    auto &f = *static_cast<WrappedCallbackFunction<ReportType> *>(userdata);
    f(timestamp, report);
}
template <typename ReportType> struct CallbackType_impl {
    typedef void (*type)(void *, const OSVR_TimeValue *, const ReportType *);
};
template <typename ReportType>
using CallbackType = typename CallbackType_impl<ReportType>::type;

template <typename ReportType> inline CallbackType<ReportType> getCaller() {
    return &callbackCaller<ReportType>;
}

template <typename ReportType, typename F>
inline std::pair<CallbackType<ReportType>, WrappedCallbackPtr<ReportType>>
wrapCallback(F &&f) {
    auto functor = WrappedCallbackPtr<ReportType>{
        new WrappedCallbackFunction<ReportType>{std::forward<F>(f)}};
    return std::make_pair(getCaller<ReportType>(), std::move(functor));
}
} // namespace detail
using detail::wrapCallback;

using namespace osvr::util;

static const OSVR_ChannelCount FUSED_SENSOR_ID = 0;
static const OSVR_ChannelCount TRANSFORMED_VIDEO_SENSOR_ID = 1;

VideoIMUFusion::VideoIMUFusion(OSVR_PluginRegContext ctx,
                               std::string const &name,
                               std::string const &imuPath,
                               std::string const &videoPath) {
    /// Create the initialization options
    OSVR_DeviceInitOptions opts = osvrDeviceCreateInitOptions(ctx);

    osvrDeviceTrackerConfigure(opts, &m_trackerOut);

    /// Create the device token with the options
    OSVR_DeviceToken dev;
    if (OSVR_RETURN_FAILURE ==
        osvrAnalysisSyncInit(ctx, name.c_str(), opts, &dev, &m_clientCtx)) {
        throw std::runtime_error("Could not initialize analysis plugin!");
    }
    m_dev = osvr::pluginkit::DeviceToken(dev);

    /// Send JSON descriptor
    m_dev.sendJsonDescriptor(org_osvr_filter_videoimufusion_json);

    /// Register update callback
    m_dev.registerUpdateCallback(this);

    /// Set up to receive our input.
    osvrClientGetInterface(m_clientCtx, imuPath.c_str(), &m_imu);
    auto imuCb = wrapCallback<OSVR_OrientationReport>([&](
        const OSVR_TimeValue *timestamp, const OSVR_OrientationReport *report) {
        handleIMUData(*timestamp, *report);
    });
    osvrRegisterOrientationCallback(m_imu, imuCb.first, imuCb.second.get());
    m_imuCb = std::move(imuCb.second);

    osvrClientGetInterface(m_clientCtx, videoPath.c_str(), &m_videoTracker);
    auto videoTrackerCb = wrapCallback<OSVR_PoseReport>(
        [&](const OSVR_TimeValue *timestamp, const OSVR_PoseReport *report) {
            handleVideoTrackerData(*timestamp, *report);
        });
    osvrRegisterPoseCallback(m_videoTracker, videoTrackerCb.first,
                             videoTrackerCb.second.get());
    m_videoTrackerCb = std::move(videoTrackerCb.second);

    enterCameraPoseAcquisitionState();
}

VideoIMUFusion::~VideoIMUFusion() {
    /// free the interfaces before the pointed-to function objects disappear.
    if (m_imu) {
        osvrClientFreeInterface(m_clientCtx, m_imu);
        m_imu = nullptr;
    }
    if (m_videoTracker) {
        osvrClientFreeInterface(m_clientCtx, m_videoTracker);
        m_imu = nullptr;
    }
}

OSVR_ReturnCode VideoIMUFusion::update() { return OSVR_RETURN_SUCCESS; }

static inline double secondsElapsed(const OSVR_TimeValue &earlier,
                                    const OSVR_TimeValue &later) {

    double dt = (later.microseconds - earlier.microseconds) / 1000000.0 +
                (later.seconds - earlier.seconds);
    return dt;
}

static const double InitialStateError[] = {1., 1., 1., 1., 1., 1.,
                                           1., 1., 1., 1., 1., 1.};
static const double IMUError[] = {1., 1.5, 1.};
static const double CameraOrientationError[] = {1.1, 1.1, 1.1};
static const double CameraPositionError[] = {1., 1., 1.};
using osvr::kalman::types::Vector;
class VideoIMUFusion::RunningData {
  public:
    RunningData(Eigen::Isometry3d const &cTr,
                OSVR_OrientationState const &initialIMU,
                OSVR_PoseState const &initialVideo,
                OSVR_TimeValue const &lastPosition,
                OSVR_TimeValue const &lastIMU)
        : m_filter(), m_cTr(cTr), m_orientation(fromQuat(initialIMU)),
          m_lastPosition(lastPosition), m_lastIMU(lastIMU) {

        Eigen::Isometry3d roomPose = takeCameraPoseToRoom(initialVideo);
        osvr::kalman::types::DimVector<FilterState> initialState =
            osvr::kalman::types::DimVector<FilterState>::Zero();
        using namespace osvr::kalman::pose_externalized_rotation;
        position(initialState) = roomPose.translation();
        m_filter.state().setStateVector(initialState);
        m_filter.state().setQuaternion(Eigen::Quaterniond(roomPose.rotation()));
        m_filter.state().setErrorCovariance(
            Vector<12>(InitialStateError).asDiagonal());

        m_filter.processModel().noiseAutocorrelation *= 0.5;

        // Very crudely set up some error estimates.
        using osvr::kalman::external_quat::vecToQuat;
        using osvr::kalman::external_quat::getVec4;
        m_imuError = getVec4(vecToQuat(Vector<3>::Map(IMUError)));
        m_cameraError.head<3>() = Vector<3>::Map(CameraPositionError);
        m_cameraError.tail<4>() =
            getVec4(vecToQuat(Vector<3>::Map(CameraOrientationError)));
    }

    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
    void handleIMUReport(const OSVR_TimeValue &timestamp,
                         const OSVR_OrientationReport &report) {
        /// Right now, just accepting the orientation report as it is. This
        /// does not correct for gyro drift.
        m_orientation = fromQuat(report.rotation);

        if (preReport(timestamp, m_lastIMU)) {
            AbsoluteOrientationMeasurement meas{fromQuat(report.rotation),
                                                m_imuError.asDiagonal()};
            m_filter.correct(meas);
        }
    }
    void handleVideoTrackerReport(const OSVR_TimeValue &timestamp,
                                  const OSVR_PoseReport &report) {
        Eigen::Isometry3d roomPose = takeCameraPoseToRoom(report.pose);

        if (preReport(timestamp, m_lastPosition)) {
            Eigen::Quaterniond ori(roomPose.rotation());
            Eigen::Vector3d pos(roomPose.translation());
            AbsolutePoseMeasurement meas{pos, ori, m_cameraError.asDiagonal()};
            m_filter.correct(meas);
            std::cout << "State: " << m_filter.state().stateVector().transpose()
                      << "\n";
            std::cout << "Quat: "
                      << osvr::kalman::external_quat::getVec4(
                             m_filter.state().getQuaternion())
                             .transpose()
                      << "\n";
            std::cout << "Error:\n";
            std::cout << m_filter.state().errorCovariance() << std::endl;
        }
    }

    /// Returns true if we succeeded and can filter in some data.
    bool preReport(const OSVR_TimeValue &timestamp, OSVR_TimeValue &last) {
        auto dt = secondsElapsed(last, timestamp);
        if (dt <= 0) {
            return false;
        }

        last = timestamp;
        m_filter.predict(dt);
        return true;
    }
#if 0
    Eigen::Quaterniond const &getOrientation() const { return m_orientation; }
    Eigen::Vector3d const &getPosition() const {
        return positionFilter.getState();
    }
#else
    Eigen::Quaterniond getOrientation() const {
        return m_filter.state().getQuaternion();
    }
    Eigen::Vector3d getPosition() const {
        return m_filter.state().getPosition();
    }
#endif

    Eigen::Isometry3d takeCameraPoseToRoom(OSVR_PoseState const &pose) {
        return m_cTr * fromPose(pose);
    }

  private:
    Filter m_filter;
    Vector<4> m_imuError;
    Vector<7> m_cameraError;
    const Eigen::Isometry3d m_cTr;
    Eigen::Quaterniond m_orientation;
    OSVR_TimeValue m_lastPosition;
    OSVR_TimeValue m_lastIMU;
};

void VideoIMUFusion::enterRunningState(Eigen::Isometry3d const &cTr) {
    m_cTr = cTr;
    std::cout << "Camera is located in the room at roughly "
              << m_cTr.translation().transpose() << std::endl;
    m_state = State::Running;
    auto oriTs = OSVR_TimeValue{};
    auto imuState = OSVR_OrientationState{};
    auto ret = osvrGetOrientationState(m_imu, &oriTs, &imuState);
    BOOST_ASSERT_MSG(ret == OSVR_RETURN_SUCCESS,
                     "Must have one IMU report by now!");
    auto posTs = OSVR_TimeValue{};
    auto videoState = OSVR_PoseState{};
    ret = osvrGetPoseState(m_videoTracker, &posTs, &videoState);
    BOOST_ASSERT_MSG(ret == OSVR_RETURN_SUCCESS,
                     "Must have one video report by now!");

    m_runningData.reset(new VideoIMUFusion::RunningData(
        cTr, imuState, videoState, posTs, oriTs));
    /// @todo should we just let it hang around instead of releasing memory in a
    /// callback?
    m_startupData.reset();
}

void VideoIMUFusion::handleIMUData(const OSVR_TimeValue &timestamp,
                                   const OSVR_OrientationReport &report) {
    if (m_state != State::Running) {
        return;
    }
    m_runningData->handleIMUReport(timestamp, report);

    // send a pose report
    auto newPose = OSVR_PoseState{};
    toQuat(m_runningData->getOrientation(), newPose.rotation);
    vecMap(newPose.translation) = m_runningData->getPosition();
    osvrDeviceTrackerSendPoseTimestamped(m_dev, m_trackerOut, &newPose,
                                         FUSED_SENSOR_ID, &timestamp);
}
void VideoIMUFusion::handleVideoTrackerData(const OSVR_TimeValue &timestamp,
                                            const OSVR_PoseReport &report) {
    if (m_state == State::AcquiringCameraPose) {
        handleVideoTrackerDataDuringStartup(timestamp, report);
        return;
    }
    m_runningData->handleVideoTrackerReport(timestamp, report);
#if 0
    // Not issuing a new main output here, let the IMU trigger that.
#else

    // send a pose report
    auto newPose = OSVR_PoseState{};
    toQuat(m_runningData->getOrientation(), newPose.rotation);
    vecMap(newPose.translation) = m_runningData->getPosition();
    osvrDeviceTrackerSendPoseTimestamped(m_dev, m_trackerOut, &newPose,
                                         FUSED_SENSOR_ID, &timestamp);
#endif
    // However, for debugging, we will output a second sensor that is just the
    // video tracker data re-oriented.
    auto videoPose = m_runningData->takeCameraPoseToRoom(report.pose);
    auto newDebugPose = OSVR_PoseState{};
    toPose(videoPose, newDebugPose);
    osvrDeviceTrackerSendPoseTimestamped(m_dev, m_trackerOut, &newDebugPose,
                                         TRANSFORMED_VIDEO_SENSOR_ID,
                                         &timestamp);
}

class VideoIMUFusion::StartupData {
  public:
    StartupData()
        : last(time::getNow()), positionFilter(filters::one_euro::Params{}),
          orientationFilter(filters::one_euro::Params{}) {}
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
    void handleReport(const OSVR_TimeValue &timestamp,
                      const OSVR_PoseReport &report,
                      const OSVR_OrientationState &orientation) {
        auto dt = secondsElapsed(last, timestamp);
        if (dt <= 0) {
            dt = 1; // in case of weirdness, avoid divide by zero.
        }
        // tranform from camera to tracked device
        auto dTc = fromPose(report.pose);
        // orientation is dTr: room to tracked device
        // cTr is room to camera, so we can take camera-reported dTc * cTr and
        // get dTr with position...
        Eigen::Isometry3d cTr =
            dTc.inverse() * Eigen::Isometry3d(fromQuat(orientation));
        positionFilter.filter(dt, cTr.translation());
        orientationFilter.filter(dt, Eigen::Quaterniond(cTr.rotation()));
        ++reports;
        last = timestamp;
    }

    bool finished() const { return reports >= REQUIRED_SAMPLES; }

    Eigen::Isometry3d getRoomToCamera() const {
        Eigen::Isometry3d ret;
        ret.fromPositionOrientationScale(positionFilter.getState(),
                                         orientationFilter.getState(),
                                         Eigen::Vector3d::Constant(1));
        return ret;
    }

  private:
    static const std::size_t REQUIRED_SAMPLES = 10;
    std::size_t reports = 0;
    OSVR_TimeValue last;

    filters::OneEuroFilter<Eigen::Vector3d> positionFilter;
    filters::OneEuroFilter<Eigen::Quaterniond> orientationFilter;
};
void VideoIMUFusion::enterCameraPoseAcquisitionState() {
    m_startupData.reset(new VideoIMUFusion::StartupData);
    m_state = State::AcquiringCameraPose;
}
void VideoIMUFusion::handleVideoTrackerDataDuringStartup(
    const OSVR_TimeValue &timestamp, const OSVR_PoseReport &report) {
    auto lastIMU = OSVR_TimeValue{};
    auto imuState = OSVR_OrientationState{};
    auto ret = osvrGetOrientationState(m_imu, &lastIMU, &imuState);
    if (ret != OSVR_RETURN_SUCCESS) {
        // We don't yet have a state for the IMU, remarkably, so we'll wait
        // until next time.
        return;
    }
    m_startupData->handleReport(timestamp, report, imuState);
    if (m_startupData->finished()) {
        enterRunningState(m_startupData->getRoomToCamera());
    }
}