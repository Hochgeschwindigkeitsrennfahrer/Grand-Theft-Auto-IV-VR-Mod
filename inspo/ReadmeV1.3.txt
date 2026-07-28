First Person v1.3 - By C06alt
__________________________


-- Update


EFLC weapon support.
Special HMD mode for Oculus rift support.


-- Default Keys

B - Cruise control
N - Mouse steering
M - Camera lock
L - Retro camera

All keys can be reassigned in the ini file or disabled by assigning them to 0

-- First Person

!! IMPORTANT !!

To enter first person view in a car cycle the views with the view button or key until it comes up.
To enter first person on foot double tap the view button or key quickly while on foot.


-Joypad
On a regular pad right analog stick looks around in a similar way to the games camera.
After 2 seconds the view will auto center in a vehicle if not adjusted in that time.
Rear view and cinematic buttons function as expected.

-Mouse
Looking around is done with the mouse similar to the games camera.
If caps lock is pressed and released the mouse will steer the vehicle and the view will center.
Rear view functions as expected.

-- Cruise control

Cruise control will deactivate if you drive to harshly or have a collision.
When cruising you can adjust your speed with the throttle or break or hand break.

-- Camera lock

When the camera is locked moving the mouse or look axis of a joypad will disable the lock for
2 seconds or the value in milliseconds for CenterTime.
Camera lock also disables automatically during drive by shooting.

-- INI settings

-Note: you can safely bind to letter keys as inputs are disabled when a chat box is open.

[Settings]
FPX = 0 		If you wanted to move the position of the camera from the center of both eyes over nicos right eye you would enter 3 here.
FPY = 0			If you wanted to move the position of the camera away from nicos face by about 6 inches you would enter 6 here.
FPZ = 0 		If you wanted to lower the camera below nicos eyes by 3 inches you would enter -3 here.
HMD = 1			This value when set to 1 allows the oculus rift to control the headlook movement
HMDPhoneDistance = 180	This value determines how far away the special Rift phone is form your face.
ForwardFOV = 60		This is the forward facing main camera field of view value, a value of 10 would be extreme zoom while a value of 120 would be fisheye.
FootFOV = 60 		This is the forward facing main camera on foot, field of view value, a value of 10 would be extreme zoom while a value of 120 would be fisheye.
RearFOV = 80 		This is the rear view mirror camera field of view value, a value of 10 would be extreme zoom while a value of 120 would be fisheye.
JoySensLev1 = 50 	The first Joystick view sensitivity range extends nearly to the edge of the analog devices range, Strength 1 - 100.
JoySensLev2 = 50 	The second Joystick view sensitivity range extends from nearly the absolute edge of the devices range to infinity,Strength range is 1 - 100.
MouseSens = 50 		The mouse can be made more or less sensitive in viewing mode by lowering or raising this number 1- 100
BikeSteerSens = 50 	Adjusting this value you can make the bikes more or less sensitive to mouse input when in mouse steering mode 1 - 100
CarSteerSens = 50 	Adjusting this value you can make the cars more or less sensitive to mouse input when in mouse steering mode 1 - 100
MotionBlur = 0		A higher number will result in more motion blur.
CenterTime = 2000 	The time in milliseconds before the camera should re center itself if no input is detected.
AutoCenterLR = 1 	Choose if the camera should auto center left to right.
AutoCenterUD = 1 	Choose if the camera should auto center up and down.
LockHorizonLR = 1       Using 0 for this value will make the horizon tilt with the camera
LockHorizonUD = 1       Using 0 for this value will make the horizon rise and dip with the vehicle.
RetroKey = 76		Key to activate the Retro view.
CruiseControlKey = 66	Key to activate cruise control.
MouseSteerKey = 78	Key to activate mouse steering.
CamLockKey = 77		Key to lock the 3rd person driving camera behind the vehicle.



[Crosshair]
CenterDotR = 255	Value of the color red in the center dot of the crosshair.
CenterDotG = 255	Value of the color green in the center dot of the crosshair.
CenterDotB = 255	Value of the color blue in the center dot of the crosshair.
CenterDotA = 155	Value of the alpha in the center dot of the crosshair.
RingR = 255		Value of the color red in the outer ring of the crosshair.
RingG = 255		Value of the color green  in the outer ring of the crosshair.
RingB = 255		Value of the color blue in the outer ring of the crosshair.
RingA = 155		Value of the color red in the outer ring of the crosshair.
RingS = 15		Value of the color alpha in the outer ring of the crosshair.


::Credits::

Thanks to aru specifically for the c++ script hook and SparkIV.
Id also like to thank all the address documenters.
And Lieutenant Cain and Cylonsurfer for bug testing.
