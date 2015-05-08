/**
 * @file object_tracker.cpp
 * @brief The object tracking code.
 */

#include "common.h"
#include "object_tracker.h"

using namespace picopter;
using namespace picopter::navigation;
using std::chrono::steady_clock;
using std::chrono::milliseconds;
using std::chrono::microseconds;
using std::chrono::seconds;
using std::chrono::duration_cast;
using std::this_thread::sleep_for;

static void printFlightData(FlightData*);

ObjectTracker::ObjectTracker(Options *opts, int camwidth, int camheight, TrackMethod method)
: m_camwidth(camwidth)
, m_camheight(camheight)
, m_pidx(0,0,0,0.03)
, m_pidy(0,0,0,0.03)
, m_track_method{method}
, SEARCH_GIMBAL_LIMIT(60)
{
    Options clear;
    if (!opts) {
        opts = &clear;
    }
    
    //Observation mode: Don't actually send the commands to the props
    opts->SetFamily("GLOBAL");
    m_observation_mode = opts->GetBool("OBSERVATION_MODE", false);

    //The gain has been configured for a 320x240 image, so scale accordingly.
    opts->SetFamily("OBJECT_TRACKER");
    TRACK_TOL = opts->GetInt("TRACK_TOL", m_camwidth/7);
    TRACK_Kpx = opts->GetReal("TRACK_Kpx", 10 * 320.0/m_camwidth);
    TRACK_Kpy = opts->GetReal("TRACK_Kpy", 10 * 320.0/m_camwidth);
    TRACK_TauI = opts->GetReal("TRACK_TauI", 3.8);
    TRACK_TauD = opts->GetReal("TRACK_TauD", 0.0000006);
    TRACK_SPEED_LIMIT_X = opts->GetInt("TRACK_SPEED_LIMIT_X", 65);
    TRACK_SPEED_LIMIT_Y = opts->GetInt("TRACK_SPEED_LIMIT_Y", 45);
    TRACK_SETPOINT_X = opts->GetReal("TRACK_SETPOINT_X", 0);
    //We bias the vertical limit to be higher due to the pitch of the camera.
    TRACK_SETPOINT_Y = opts->GetReal("TRACK_SETPOINT_Y", -m_camheight/15);
    
    m_pidx.SetTunings(TRACK_Kpx, TRACK_TauI, TRACK_TauD);
    m_pidx.SetInputLimits(-m_camwidth/2, m_camwidth/2);
    m_pidx.SetOutputLimits(-TRACK_SPEED_LIMIT_X, TRACK_SPEED_LIMIT_X);
    m_pidx.SetSetPoint(TRACK_SETPOINT_X);
    
    m_pidy.SetTunings(TRACK_Kpy, TRACK_TauI, TRACK_TauD);
    m_pidy.SetInputLimits(-m_camheight/2, m_camheight/2);
    m_pidy.SetOutputLimits(-TRACK_SPEED_LIMIT_Y, TRACK_SPEED_LIMIT_Y);
    m_pidy.SetSetPoint(TRACK_SETPOINT_Y);
}

ObjectTracker::ObjectTracker(int camwidth, int camheight, TrackMethod method)
: ObjectTracker(NULL, camwidth, camheight, method) {}

ObjectTracker::~ObjectTracker() {
    
}

ObjectTracker::TrackMethod ObjectTracker::GetTrackMethod() {
    return m_track_method.load(std::memory_order_relaxed);
}

void ObjectTracker::SetTrackMethod(TrackMethod method) {
    Log(LOG_INFO, "Track method: %d", method);
    m_track_method.store(method, std::memory_order_relaxed);
}

void ObjectTracker::Run(FlightController *fc, void *opts) {
    if (fc->cam == NULL) {
        Log(LOG_WARNING, "Not running object detection - no usable camera!");
        return;
    } /*else if (!fc->gps->WaitForFix(30)) {
        Log(LOG_WARNING, "Not running object detection - no GPS fix!");
        return;
    }*/
    
    Log(LOG_INFO, "Object detection initiated; awaiting authorisation...");
    SetCurrentState(fc, STATE_AWAITING_AUTH);
    if (!fc->WaitForAuth()) {
        Log(LOG_INFO, "All stop acknowledged; quitting!");
        return;
    }
    
    Log(LOG_INFO, "Authorisation acknowledged. Finding object to track...");
    SetCurrentState(fc, STATE_TRACKING_SEARCHING);
    
    Point2D detected_object = {0,0};
    Point3D object_body_coords = {0,0,0};  //location of the target in body coordinates

    std::vector<Point2D> locations;
    FlightData course;
    GPSData gps_position;
    auto last_fix = steady_clock::now() - seconds(2);
    bool had_fix = false;

    while (!fc->CheckForStop()) {
        double update_rate = 1.0 / fc->cam->GetFramerate();
        auto sleep_time = microseconds((int)(1000000*update_rate));

        fc->cam->GetDetectedObjects(&locations);
        fc->fb->GetData(&course);
        fc->gps->GetLatest(&gps_position);
        
        //Do we have an object?
        if (locations.size() > 0) {                                             
            detected_object = locations.front();
            SetCurrentState(fc, STATE_TRACKING_LOCKED);
            
            //Set PID update intervals
            m_pidx.SetInterval(update_rate);
            m_pidy.SetInterval(update_rate);
            
            //Determine trajectory to track the object (PID control)
            EstimatePositionFromImageCoords(&gps_position, &course, &detected_object, &object_body_coords);

            if (!m_observation_mode) {
                CalculateTrackingTrajectory(fc, &course, &object_body_coords, true);
                fc->fb->SetData(&course);
            }
            printFlightData(&course);
            last_fix = steady_clock::now();
            had_fix = true;
        } else if (had_fix && steady_clock::now() - last_fix < seconds(2)) {
            //We had an object, attempt to follow according to last known position, but slower
            //Set PID update intervals
            m_pidx.SetInterval(update_rate);
            m_pidy.SetInterval(update_rate);
            
            m_pidx.Reset();
            m_pidy.Reset();
            
            //Determine trajectory to track the object (PID control)
            if (!m_observation_mode) {
                CalculateTrackingTrajectory(fc, &course, &object_body_coords, false);
                fc->fb->SetData(&course);
            }
            printFlightData(&course);
        } else {
            //Object lost; we should do a search pattern (TBA)
            SetCurrentState(fc, STATE_TRACKING_SEARCHING);
            
            //Reset the accumulated error in the PIDs
            m_pidx.Reset();
            m_pidy.Reset();
            //m_pid_yaw.Reset();

            //fc->fb->Stop();
            if (had_fix) {
                fc->cam->SetArrow({0,0});
                Log(LOG_WARNING, "No object detected. Idling.");
                had_fix = false;
            }
        }
        
        sleep_for(sleep_time);
    }
    fc->cam->SetArrow({0,0});
    Log(LOG_INFO, "Object detection ended.");
    fc->fb->Stop();
}

//create the body coordinate vector for the object in the image
//in the absence of a distance sensor, we're assuming the object is on the ground, at the height we launched from.
void ObjectTracker::EstimatePositionFromImageCoords(GPSData *pos, FlightData *current, Point2D *object_location, Point3D *object_position){
    #warning "using 8m as ground level"
    double launchAlt = 8.0; //the James Oval is about 8m above sea level
    double heightAboveTarget = std::max(pos->fix.alt - launchAlt, 0.0);

    #warning "using 0.8m as height above target"
    heightAboveTarget = 0.8;    //hard-coded for lab test

    //Calibration factor
    double L = 2587.5 * m_camwidth/2592.0;
    double gimbalVertical = 37; //gimbal angle that sets the camera to point straight down

    //Angles from image normal
    double theta = atan(object_location->y/L); //y angle
    double phi   = atan(object_location->x/L); //x angle
    
    //Tilt - in radians from vertical
    double gimbalTilt = DEG2RAD(gimbalVertical - current->gimbal);
    double objectAngleY = gimbalTilt + theta;
    double forwardPosition = tan(objectAngleY) * heightAboveTarget;
    double lateralPosition = heightAboveTarget * (object_location->x / L) / cos(objectAngleY);

    object_position->y = forwardPosition;
    object_position->x = lateralPosition;
    object_position->z = heightAboveTarget;

    Log(LOG_INFO, "HAT: %.1f m, X: %.2fdeg, Y: %.2fdeg, FP: %.2fm, LP: %.2fm",
        heightAboveTarget, RAD2DEG(phi), RAD2DEG(objectAngleY), forwardPosition, lateralPosition);
}

void ObjectTracker::CalculateTrackingTrajectory(FlightController *fc, FlightData *course, Point3D *object_position, bool has_fix) {
    //Zero the course commands
    memset(course, 0, sizeof(FlightData));
    
    double desiredSlope = 1;
    double desiredForwardPosition = object_position->z/desiredSlope;
    
    //need to add a way of passing target bearings over here so we don't re-compute, can I use a Point2D?
    double phi = atan(object_position->x/object_position->y);   //check this, I think the form changed.

    m_pidx.SetProcessValue(-phi);   //rename this to m_pid_yaw or something, we can seriously use an X controller in chase now.
    m_pidy.SetProcessValue(-object_position->y + desiredForwardPosition);

    /*    //Now we can take full advantage of this omnidirectional platform.
    objectDistance = sqrt(object_position->x*object_position->x + object_position->y*object_position->y);
    distanceError = objectDistance - desiredForwardPosition;
    m_pid_yaw.SetProcessValue(-phi);
    m_pidx.SetProcessValue(-object_position->x * (distanceError/objectDistance) );  //the place we want to put the copter, relative to the copter.
    m_pidy.SetProcessValue(-object_position->y * (distanceError/objectDistance) );
    */

    double trackx = m_pidx.Compute(), tracky = m_pidy.Compute();
    
    //We've temporarily lost the object, reduce speed slightly
    //We should probably skip all of the above computations so we don't corrupt the PIDs if we don't have a fix.
    if (!has_fix) {
        trackx *= 0.75;
        tracky *= 0.75;
    }
    
    //this looks silly now.
    course->elevator = tracky;
    course->rudder = trackx;
    
    //Fix the angle for now...
    course->gimbal = 37;
    fc->cam->SetArrow({100*trackx/TRACK_SPEED_LIMIT_X, -100*tracky/TRACK_SPEED_LIMIT_Y});

}

void printFlightData(FlightData* data) {
    printf("A: %03d E: %03d R: %03d G: %03d\r",
        data->aileron, data->elevator, data->rudder, data->gimbal);
    fflush(stdout);
}
