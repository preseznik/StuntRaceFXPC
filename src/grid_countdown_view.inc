namespace {
// Live Super FX camera poses, relative to the player's starting pose, through
// the race-three fly-around. All three courses use the same camera path.
std::uint8_t grid_camera(const Renderer& renderer,const std::array<float,3>& origin,
                         float heading,unsigned phase,float chase_weight) {
    struct Key {unsigned phase; float x,y,z,pitch,yaw;};
    static constexpr Key keys[]{
        {1,-500,-2000,5999,-3328,31920},{2,-500,-2000,5999,-3328,31920},{3,-499,-1985,5968,-3296,31920},
        {5,-493,-1915,5828,-3264,31920},{7,-482,-1803,5603,-3232,31888},{9,-469,-1658,5313,-3120,31888},
        {11,-453,-1489,4974,-3008,31840},{13,-435,-1301,4598,-2848,31808},{15,-416,-1098,4193,-2624,31760},
        {18,-385,-774,3546,-2192,31680},{21,-365,-520,2981,-1760,31520},{23,-362,-399,2622,-1536,31360},
        {25,-365,-307,2274,-1376,31120},{28,-377,-207,1768,-1168,30608},{30,-390,-160,1438,-1088,30032},
        {31,-397,-140,1275,-1040,29648},{33,-413,-108,950,-1040,28512},{34,-422,-95,789,-1088,27680},
        {35,-431,-84,629,-1136,26528},{36,-439,-74,489,-1136,25168},{38,-441,-57,259,-1136,21904},
        {41,-420,-39,11,-928,16624},{43,-392,-31,-106,-768,13648},{44,-375,-28,-154,-688,12336},
        {46,-336,-22,-233,-528,10080},{49,-267,-16,-318,-352,7248},{53,-168,-11,-391,-240,4192},
        {55,-130,-10,-415,-192,3152},{58,-90,-13,-440,-272,2080},{63,-50,-25,-466,-528,1088},
        {68,-29,-44,-479,-928,608},{70,-15,-79,-488,-1168,272},{76,-4,-143,-497,-2398,80},
        {81,0,-155,-500,-2905,0},{88,0,-160,-500,-3141,0},{100,0,-160,-500,-3184,0}
    };
    const auto at=std::clamp(phase,keys[0].phase,std::end(keys)[-1].phase);
    const auto* b=std::lower_bound(std::begin(keys)+1,std::end(keys),at,
        [](const Key& key,unsigned value){return key.phase<value;});
    const auto& a=b[-1];
    const float t=float(at-a.phase)/(b->phase-a.phase),dx=std::lerp(a.x,b->x,t),dz=std::lerp(a.z,b->z,t);
    const auto eye=renderer.normalize_world({origin[0]+std::cos(heading)*dx-std::sin(heading)*dz,
        origin[1]+std::lerp(a.y,b->y,t),origin[2]+std::sin(heading)*dx+std::cos(heading)*dz});
    constexpr float radians=6.28318530718f/65536.f;
    const float yaw=heading-std::lerp(a.yaw,b->yaw,t)*radians;
    controls.yaw=yaw+chase_weight*std::remainder(controls.yaw-yaw,65536*radians);
    controls.pitch=std::lerp(std::lerp(a.pitch,b->pitch,t)*radians,controls.pitch,chase_weight);
    controls.distance*=chase_weight;
    controls.zoom=std::lerp(2.f,controls.zoom,chase_weight);
    controls.target_x=std::lerp(eye[0],controls.target_x,chase_weight);
    controls.target_y=std::lerp(eye[1],controls.target_y,chase_weight);
    controls.target_z=std::lerp(eye[2],controls.target_z,chase_weight);
    return static_cast<std::uint8_t>(-(static_cast<std::uint16_t>(std::lround(-controls.yaw/radians))>>8));
}
}
