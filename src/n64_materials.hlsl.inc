R"(
#ifdef ENHANCED_SCENE
Texture2D<float4> material_tiles:register(t7);
#ifdef REMASTERED_LIGHT
SamplerState asphalt_sampler:register(s3);
// Sample along the projected road footprint. The minor axis can retain grit
// across the lane even when the pixel covers a long strip down the road.
float asphalt_grain(float2 meters,float grains_per_meter) {
    return material_tiles.Sample(asphalt_sampler,meters*(grains_per_meter/32.)).r;
}
#endif
#ifdef N64_MODE
float3 triangle_sample(float2 uv,int mip) {
    int size=32>>mip;
    float2 texel=uv*size-.5, f=frac(texel);
    int2 i=int2(floor(texel));
    int direction=1;
    if(f.x+f.y>1) {i+=1; f=1-f; direction=-1;}
    int mask=size-1;
    float3 a=material_tiles.Load(int3(i&mask,mip)).rgb;
    float3 b=material_tiles.Load(int3((i+int2(direction,0))&mask,mip)).rgb;
    float3 c=material_tiles.Load(int3((i+int2(0,direction))&mask,mip)).rgb;
    return a+(b-a)*f.x+(c-a)*f.y;
}
float3 filtered_tile(float2 uv) {
    float footprint=max(length(ddx(uv)),length(ddy(uv)))*32;
    float lod=clamp(log2(max(1,footprint)),0,5);
    int level=(int)lod;
    return lerp(triangle_sample(uv,level),triangle_sample(uv,min(level+1,5)),frac(lod));
}
#endif
float grain_hash(float2 p) {
    float3 h=frac(p.xyx*float3(.1031,.1030,.0973));
    h+=dot(h,h.yzx+33.33);
    return frac((h.x+h.y)*h.z);
}
// Fade unresolved octaves to their mean, especially on the road near the horizon.
float grain(float2 p) {
    float2 width=fwidth(p);
    float2 i=floor(p),f=frac(p);
    f=f*f*(3-2*f);
    float a=lerp(grain_hash(i),grain_hash(i+float2(1,0)),f.x);
    float b=lerp(grain_hash(i+float2(0,1)),grain_hash(i+1),f.x);
    return lerp(lerp(a,b,f.y),.5,saturate(max(width.x,width.y)-.35));
}
float3 lake_water(float2 p,float seconds) {
    float sand=grain(p*.48);
#ifdef N64_MODE
    float gravel=filtered_tile(p*.36).r;
    float ripple=filtered_tile(p*float2(.12,.36)).g;
#else
    float gravel=grain(p*5.5);
    float2 wave=p+seconds*float2(.10,-.22);
    float ripple=grain(wave*float2(.7,3.2)+1.6*grain(wave*.4));
#endif
    float3 bed=lerp(float3(.23,.43,.44),float3(.55,.66,.58),.66*sand+.34*gravel);
    return bed+float3(.075,.10,.11)*smoothstep(.56,.86,ripple);
}
// Average narrow joints as their period shrinks below a pixel at speed.
float panel_joint(float meters,float spacing,float half_width) {
    float dist=abs(frac(meters/spacing+.5)-.5)*spacing;
    float footprint=max(fwidth(meters),.001);
    return (1-smoothstep(half_width-footprint*.5,half_width+footprint*.5,dist))
           *saturate(1-footprint/(spacing*.5));
}
float painted_rect(float2 p,float2 center,float2 half_size) {
    float2 edge=abs(p-center)-half_size;
    float2 aa=max(fwidth(p),.0001);
    float2 coverage=saturate(.5-edge/aa);
    return coverage.x*coverage.y;
}
float3 building_material(float2 p,float surface,float3 tint) {
    float3 result=tint;
    if(surface==-11 || surface==-19) {
        float rib=panel_joint(p.x,1.4,.065);
        float3 roof=tint*(.92-.26*rib)+.045*(grain(p*2)-.5);
        if(surface==-19) {
            float edge=21.33-max(abs(p.x),abs(p.y));
            roof=lerp(roof,float3(.69,.20,.12),1-smoothstep(.55,.67,edge));
        }
        result=roof;
    } else if(surface==-12 || surface==-14) {
        // Stagger roof courses and brick headers by half a tile on alternate rows.
        float2 block=surface==-12?float2(1.2,.63):float2(.82,.40);
        float row=floor(p.y/block.y);
        float u=p.x+.5*block.x*fmod(abs(row),2);
        float joint=max(panel_joint(u,block.x,.035),panel_joint(p.y,block.y,.035));
        float wear=.08*(grain(p*2.5)-.5)+.10*(grain(float2(u/block.x,row))-.5);
        result=lerp(tint+wear,surface==-12?tint*.55:float3(.62,.54,.45),joint);
    } else if(surface==-13 || surface==-18 || surface==-20) {
        float3 wall=tint*(1-.19*panel_joint(p.y,.53,.045))+.045*(grain(p*2)-.5);
        wall=lerp(float3(.45,.43,.38),wall,smoothstep(.52,.60,p.y));
        float2 bay=float2(p.x-6.4*round(p.x/6.4),p.y);
        if(surface==-18) {
            float shutter=painted_rect(abs(p),float2(5.2,1.68),float2(3.65,1.55));
            result=lerp(wall,float3(.45,.53,.50)*(1-.18*panel_joint(p.y,.36,.025)),shutter);
        } else {
            float2 center=float2(0,4.85);
            float window_bay=surface==-13?step(3.2,abs(p.x)):1;
            float frame=painted_rect(bay,center,float2(1.45,1.72))*window_bay;
            float glass=painted_rect(bay,center,float2(1.23,1.49))*window_bay;
            float bar=max(painted_rect(bay,center,float2(.07,1.52)),painted_rect(bay,center,float2(1.27,.08)));
            wall=lerp(wall,float3(.90,.84,.71),frame);
            wall=lerp(wall,lerp(float3(.12,.23,.26),float3(.39,.56,.58),saturate(.27*bay.y-.63+.09*bay.x)),glass*(1-bar));
            if(surface==-13) {
                float door=painted_rect(p,float2(0,2.55),float2(1.36,2.55));
                wall=lerp(wall,float3(.31,.36,.32),door);
                wall=lerp(wall,float3(.45,.58,.59),painted_rect(p,float2(0,3.55),float2(.90,.88)));
                wall=lerp(wall,float3(.92,.78,.52),painted_rect(p,float2(.96,2.03),float2(.16,.10)));
            }
            float vent=painted_rect(p,float2(0,10.65),float2(.85,.62));
            result=lerp(wall,float3(.34,.36,.31)+.18*panel_joint(p.y,.22,.035),vent);
        }
    } else if(surface==-16) {
        float joint=max(panel_joint(p.x,5.4,.05),panel_joint(p.y,5.4,.05));
        float3 concrete=float3(.55,.54,.49)-.11*joint+.075*(grain(p*4)-.5);
        float2 island=abs(p)-float2(10.67,9.78);
        float border=painted_rect(island,0,float2(4.2,1.65))-painted_rect(island,0,float2(3.95,1.40));
        result=lerp(concrete,float3(.93,.72,.32),border);
    } else {
        // Each dispenser has an enamel plinth, cream upper casing and a black display bezel.
        float3 pump=lerp(tint,float3(.84,.81,.68),smoothstep(.44,.46,p.y));
        pump=lerp(pump,float3(.19,.22,.22),1-smoothstep(.06,.08,p.y));
        if(surface==-15) {
            float2 screen=float2(.41,.72);
            pump=lerp(pump,float3(.13,.18,.18),painted_rect(p,screen,float2(.24,.15)));
            pump=lerp(pump,float3(.51,.65,.58),painted_rect(p,screen,float2(.20,.09)));
            float2 digit=float2(frac((p.x-.22)/.085)-.5,p.y-.72);
            float segments=painted_rect(digit,0,float2(.32,.057))-painted_rect(digit,0,float2(.18,.039));
            segments=max(segments,painted_rect(digit,0,float2(.28,.009)));
            pump=lerp(pump,float3(.12,.18,.17),segments*painted_rect(p,screen,float2(.17,.08)));
            // A U-shaped hose returns to the nozzle beside the display.
            float2 hose=(p-float2(.82,.39))*float2(1,0.75);
            float curve=length(float2(hose.x,min(hose.y,0)))-.075;
            float ink=1-smoothstep(.010,.014+fwidth(curve),abs(curve));
            pump=lerp(pump,float3(.13,.16,.15),ink*painted_rect(p,float2(.82,.44),float2(.11,.24)));
            pump=lerp(pump,float3(.21,.35,.26),painted_rect(p,float2(.895,.63),float2(.034,.082)));
        }
        result=pump+.026*(grain(p*42)-.5);
    }
    return result;
}
float3 track_material(float3 uv,float3 original) {
    float2 p=uv.xy;
    if(uv.z < -9.5) {
        float ripple=grain(p*float2(.45,1.9))-.5;
        return float3(.37,.59,.60)+.055*ripple;
    }
    if(uv.z < -8.5) {
        // World-space meadow patches read across the whole hillside. Smaller
        // grass and rock detail fades with the screen footprint in grain().
        float patches=grain(p*.082);
        float pasture=smoothstep(.20,.80,.52*patches+.48*grain(p*.29));
#ifdef N64_MODE
        float tufts=filtered_tile(p*.30).g-.5;
#else
        float tufts=.55*grain(p*float2(1.1,1.7))+.45*grain(p*float2(3.7,5.2))-.5;
#endif
        float3 grass=lerp(float3(.22,.34,.19),float3(.55,.59,.31),pasture)+.16*tufts;
        // Exposed seams break up the grass on the steeper hill faces.
        float bed=grain(float2(.19*p.x+.31*p.y,.59*p.y-.22*p.x)+1.8*patches);
        float stone=smoothstep(.32,.62,patches)*(1-smoothstep(.05,.30,abs(bed-.5)))*original.b;
        grass=lerp(grass,float3(.52,.49,.39)+.15*tufts,stone);
        return lerp(grass*original.r,float3(.70,.65,.49)+.085*tufts,original.g);
    }
    if(uv.z < -7.5) {
        float panel=panel_joint(p.y,5,.045);
        return lerp(float3(.30,.35,.355),float3(.15,.19,.19),panel)+.025*(grain(p*8)-.5);
    }
    if(uv.z < -5.5) {
        float fleck=grain(p*21)-.5;
        float tint=grain(p*.65)-.5;
        float joint=panel_joint(p.y,6,.047);
        float3 concrete=float3(.48,.47,.43)+.078*fleck+.045*tint;
        concrete-=joint*.19;
        if(uv.z < -6.5)return concrete;
        float approach=smoothstep(35,42.6,abs(p.y));
        float3 asphalt=float3(.305,.322,.325)+.12*fleck+.058*tint;
        return lerp(concrete,asphalt,approach);
    }
    if(uv.z < -4.5)
        return original*(.985+.04*grain(p*.65)+.025*(grain(p*15)-.5));
    if(uv.z < -3.5) {
        float2 edge=min(p,1-p);
        float glazing=smoothstep(.015,.04,min(edge.x,edge.y));
        float reflection=smoothstep(.37,.79,p.y+.17*p.x);
        float3 tint=lerp(original*.58,float3(.48,.63,.66),reflection);
        return lerp(float3(.09,.12,.13),tint,glazing);
    }
    float coarse=grain(p*.45);
    if(uv.z> -1.5) {
#ifdef N64_MODE
        float aggregate=.24*coarse+.76*filtered_tile(p*.35).r;
#else
        float aggregate=.22*coarse+.27*asphalt_grain(p,7)+.51*asphalt_grain(p,27)
                        +.48*(grain(p*36)-.5)+.38*(asphalt_grain(p,64)-.5);
#endif
        return lerp(float3(.20,.22,.23),float3(.41,.425,.42),aggregate)
               +(original.g-.36)*.20;
    }
    if(uv.z> -2.5) {
        float concrete=.50*coarse+.35*grain(p*27)+.15*grain(p*float2(.7,8));
        return lerp(float3(.49,.47,.41),float3(.76,.74,.65),concrete);
    }
#ifdef N64_MODE
    float blades=filtered_tile(p*.30).g;
#else
    float blades=grain(p*float2(21,6));
#endif
    return lerp(float3(.21,.32,.18),float3(.49,.58,.27),.55*coarse+.45*blades)
           +(original.g-.60)*.23;
}
bool icy_road(float layer) { return abs(layer+35)<.5; }
bool gravel_road(float layer) { return abs(layer+36)<.5; }
bool palette_pavement(float layer) { return abs(layer+32)<.5 || icy_road(layer) || gravel_road(layer); }
#if ROAD_DETAIL
// Palette-tinted roads keep RGB for the original face colour and U for the
// lateral offset. Sample their grain in world metres in primary and reflected views.
float road_coordinates(inout float3 uv,float3 color,float2 world_meters) {
    if(!palette_pavement(uv.z))return color.r;
    float across=uv.x;
    uv.xy=world_meters;
    return across;
}
// Asphalt carries the signed route offset in the vertex data. Both concrete
// decks use local coordinates with U across the narrow deck and V along it.
float wheel_wear(float3 uv,float across) {
    bool concrete=uv.z < -28.5 && uv.z > -30.5;
    float lateral=(concrete?uv.x:across)+.5*(grain(uv.xy*.12)-.5);
    // The underpass widens back into the asphalt wheel paths at both ends.
    float lane_center=7.5;
    if(concrete)lane_center=uv.z < -29.5 ? 5.3 : lerp(5.3,7.5,smoothstep(32,42.6,abs(uv.y)));
    float lane=abs(lateral)-lane_center;
    return 1-smoothstep(.5,2.,abs(abs(lane)-1.75));
}
float3 detailed_road_material(float3 uv,float3 original,float across) {
    if(uv.z < -32.5) {
        // The timber runs across the span. Long growth lines meander through
        // finer fibres, with smoother wood where the tyres cross each board.
        float bend=grain(uv.xy*float2(.17,.38))-.5;
        float2 wood=float2(uv.x,uv.y+.23*bend);
        float rings=grain(wood*float2(.35,5.2))-.5;
        float fibres=grain(wood*float2(1.6,31))-.5;
        // Each native moving slab spans five 128-unit boards.
        float pitch=128./90.;
        float seam=panel_joint(uv.y+.5*pitch,pitch,.025);
        float wear=wheel_wear(uv,uv.x);
        return original*(1+.25*bend+(.44*rings+.24*fibres)*(1-.55*wear)-.09*wear-.26*seam);
    }
    float stones=grain(uv.xy*21)-.5;
    float flecks=grain(uv.xy*48)-.5;
    if(uv.z < -31.5 || uv.z > -26.5) {
        float wear=wheel_wear(uv,across);
        float mottling=grain(uv.xy*.74)-.5;
        float aggregate=(.09*(asphalt_grain(uv.xy,6)-.5)+.20*(asphalt_grain(uv.xy,21)-.5)
                         +.12*(asphalt_grain(uv.xy,48)-.5)+.08*flecks)*(1-.70*wear);
        if(uv.z < -31.5)return original*(1+.26*mottling+2.8*aggregate)*(1-.14*wear);
        return (float3(.285,.297,.30)+mottling*.084+aggregate)*(1-.14*wear);
    }
    if(uv.z < -30.5) {
        // Alternating raised lozenges on the tunnel cross ties. Fade the tread
        // into the brushed steel as its spacing approaches a pixel.
        float2 diagonal=float2(uv.x+uv.y,uv.x-uv.y);
        float tread=max(panel_joint(diagonal.x,.48,.045)*panel_joint(diagonal.y,1.2,.22),
                        panel_joint(diagonal.y,.48,.045)*panel_joint(diagonal.x+.6,1.2,.22));
        return float3(.22,.28,.29)*(1+.48*tread+.12*stones);
    }
    if(uv.z < -28.5) {
        float3 slab=track_material(float3(uv.xy,uv.z < -29.5 ? -7 : -6),original);
        float wear=wheel_wear(uv,0);
        return slab*(1-.14*wear)+(stones*.11+flecks*.07)*(1-.77*wear);
    }
    float chip=smoothstep(.4,.72,grain(uv.xy*6.7))*(uv.z < -27.5 ? .72 : .14);
    return lerp(original*.89,float3(.53,.50,.45),chip)+.09*stones+.06*flecks;
}
#endif
float3 scene_material(float3 uv,float3 original,float seconds,float across=0) {
#if ROADSIDE_SCENERY
    if(uv.z < -49.5) {
        // A mineral tide mark separates the damp lower wall from dry concrete.
        float height=uv.y-.62-.13*grain(float2(uv.x*.85,0));
        float damp=1-smoothstep(-.04,.04,height);
        original*=lerp(float3(1,1,1),float3(.32,.46,.40),damp);
        original+=.18*(1-smoothstep(.015,.065,abs(height)));
        uv.z=-43;
    }
    if(uv.z < -48.5) {
        // Broad, irregular wind grooves cross the drift. Use the same snow
        // grain as the adjacent verge underneath, including its distance filter.
        float2 wind=float2(uv.x+.38*uv.y,uv.y-.38*uv.x);
        wind.y+=1.3*grain(wind*.20);
        float grooves=smoothstep(.34,.77,grain(wind*float2(.55,2.8)));
        original*=lerp(float3(.70,.78,.86),float3(1,1,1),grooves);
        uv.z=-23;
    }
    if(uv.z < -47.5) {
        float rust=smoothstep(.52,.76,grain(uv.xy*3.1));
        return lerp(track_material(float3(uv.xy,-5),original),float3(.35,.22,.14),.72*rust);
    }
    if(abs(uv.z+46)<.1)return original; // Amber lens, lit by the car's headlights.
    if(uv.z < -44.5) {
        float stones=asphalt_grain(uv.xy,9)-.5;
        float pebbles=smoothstep(.36,.75,grain(uv.xy*2.4));
        return float3(.40,.385,.34)*(.82+.41*pebbles+.77*stones);
    }
    if(uv.z < -43.5) {
        float edge=min(uv.x,1-uv.x);
        float flange=1-smoothstep(.055,.095,edge);
        float plate=panel_joint(uv.y,3.6,.045);
        float bolt=painted_rect(float2(edge,uv.y-1.8*round(uv.y/1.8)),float2(.12,0),float2(.027,.09));
        float scuff=grain(float2(uv.x*11,uv.y*2))-.5;
        return lerp(float3(.31,.39,.42),original,.23)*(.9+.16*scuff+.34*flange-.36*plate)+.19*bolt;
    }
    if(uv.z < -42.5) {
        float joints=max(panel_joint(uv.x,5.6,.055),panel_joint(uv.y,3.4,.04));
        float streak=grain(uv.xy*float2(1.2,.15))-.5;
        float aggregate=asphalt_grain(uv.xy,8)-.5;
        return lerp(original,float3(.56,.55,.49),.43)*(.94+.25*streak+.30*aggregate-.28*joints);
    }
    if(uv.z < -40.5) {
        // Use the native canyon colour at the current stage of dusk.
        float chips=asphalt_grain(uv.xy,5)-.5;
        if(uv.z < -41.5) {
            float pebbles=smoothstep(.40,.76,grain(uv.xy*1.9));
            return original*(.76+.39*pebbles+.42*chips);
        }
        float seam=uv.y*.31+.53*grain(uv.xy*.12);
        float bedding=panel_joint(seam,1,.085);
        return original*(.93+.36*(grain(uv.xy*.7)-.5)+.29*chips-.29*bedding);
    }
    if(uv.z < -38.5) {
        bool coast=uv.z < -39.5;
        float3 grass=coast ? track_material(float3(uv.xy,-9),float3(original.rg,0))
                          : track_material(float3(uv.xy,-3),float3(1,.60,0));
        float edge=coast?original.b:original.r;
        if(edge>=1)return grass;
        float distance=edge+.13*(grain(uv.xy*.20)-.5);
        float speck=grain(uv.xy*8)-.5;
        float3 stones=float3(.48,.44,.36)+.26*speck;
        float leaves=smoothstep(.47,.74,grain(uv.xy*float2(2.7,4.1)));
        float3 litter=lerp(float3(.27,.24,.14),float3(.53,.41,.22),leaves)+.14*speck;
        return lerp(lerp(stones,litter,smoothstep(.10,.32,distance)),grass,smoothstep(.34,.93,distance));
    }
    if(uv.z < -36.5) {
        float furrows=grain(uv.xy*float2(4,.65));
        return original*(.59+.56*furrows+.28*grain(uv.xy*float2(12,2)));
    }
#endif
#if ROAD_DETAIL
    if(gravel_road(uv.z)) {
        // Coarse stones stand out between the compacted tracks in the rock field.
        float wear=wheel_wear(uv,across);
        float soil=grain(uv.xy*.65)-.5;
        float stones=smoothstep(.42,.8,grain(uv.xy*7))-.4;
        return original*(1+.30*soil+.56*stones*(1-.7*wear)-.17*wear);
    }
    if(icy_road(uv.z)) {
        // Frost collects across the lane. Tyres rub clear two smoother tracks.
        float wear=wheel_wear(uv,across);
        float drift=grain(uv.xy*float2(.38,.12));
        float frost=grain(uv.xy*float2(.9,4.5)+drift)-.5;
        float crystals=grain(uv.xy*15)-.5;
        float relief=(.25*frost+.22*crystals)*(1-.65*wear);
        return original*(.98+.14*(drift-.5)+relief-.065*wear);
    }
    if(uv.z < -33.5)return lake_water(uv.xy,seconds)*float3(.42,1.07,1.45); // Sea Breeze shallows
    if(uv.z < -25.5)return detailed_road_material(uv,original,across);
#endif
#if RAY_WET_REFLECTIONS || MATERIAL_ROUGHNESS
    if(uv.z < -24.5)return original; // Road paint.
#endif
    if(uv.z < -23.5)return lake_water(uv.xy,seconds);
    if(uv.z < -20.5) {
#ifdef N64_MODE
        float grit=filtered_tile(uv.xy*.32).r-.5;
#else
        float grit=grain(uv.xy*18)-.5;
#endif
        if(uv.z < -22.5) {
            // Wind-scoured snow follows the native bank colours and road elevation.
            float drift=grain(uv.xy*float2(.17,.045))-.5;
            return original*(.98+.14*drift+.065*grit);
        }
        if(uv.z < -21.5) {
            float strata=grain(float2(uv.x*.12,uv.y*.95+.7*grain(uv.xy*.11)))-.5;
            return original*(1+.34*strata+.14*grit);
        }
#ifdef REMASTERED_LIGHT
        grit=.32*(asphalt_grain(uv.xy,6)-.5)+.68*(asphalt_grain(uv.xy,22)-.5);
#endif
        return original*(1+.18*grit+.12*(grain(uv.xy*.62)-.5));
    }
    return uv.z < -10.5 ? building_material(uv.xy,round(uv.z),original) : track_material(uv,original);
}
#ifdef REMASTERED_LIGHT
cbuffer Atmosphere : register(b2) { float cloud_cover, extinction, rain_strength, weather_seconds; };
float golden_hour(float height) {
    return smoothstep(-.20,.02,height)*(1-smoothstep(.10,.56,height));
}
float3 sun_color(float3 sun) {
    float3 low_sun=lerp(float3(1.40,.57,.25),float3(1.34,.83,.39),smoothstep(-.4,.4,sun.x));
    return lerp(float3(1.05,1.0,.93),low_sun,golden_hour(sun.y));
}
float3 ambient_color(float3 sun) {
    float day=smoothstep(-.18,.34,sun.y);
    float3 sky_fill=lerp(float3(.20,.26,.43),float3(.55,.60,.67),day);
    float3 bounce=lerp(float3(.74,.44,.39),float3(.76,.59,.38),smoothstep(-.4,.4,sun.x));
    sky_fill=lerp(sky_fill,bounce,.80*golden_hour(sun.y));
    return lerp(sky_fill,lerp(float3(.20,.24,.34),float3(.68,.72,.76),day),.85*cloud_cover);
}
float3 mist_color(float3 sun) {
    float3 mist=lerp(float3(.13,.18,.28),float3(.67,.74,.77),smoothstep(-.18,.32,sun.y));
    return lerp(mist,float3(.92,.66,.49),golden_hour(sun.y)*(.78-.58*cloud_cover));
}
float3 air_perspective(float3 rgb,float meters,float3 sun) {
    return lerp(mist_color(sun),rgb,exp2(-extinction*max(0,meters-4)));
}
#endif
float3 coastal_sky(float3 d,float3 sun) {
    float glow=saturate(dot(d,sun));
    float3 sky=lerp(float3(.85,.86,.77),float3(.34,.62,.80),smoothstep(-.04,.31,d.y));
#ifdef REMASTERED_LIGHT
    float day=smoothstep(-.18,.32,sun.y);
    float twilight=(1-smoothstep(.06,.45,abs(sun.y)))*smoothstep(-.24,-.03,sun.y);
    float3 horizon=lerp(float3(.13,.17,.29),float3(.82,.86,.85),day);
    horizon=lerp(horizon,float3(.99,.55,.34),twilight*(.32+.68*pow(glow,4)));
    sky=lerp(horizon,lerp(float3(.035,.065,.15),float3(.29,.56,.81),day),smoothstep(-.04,.45,d.y));
    sky+=sun_color(sun)*.29*pow(glow,24)*smoothstep(-.15,.02,sun.y);
#else
    sky+=float3(.22,.14,.045)*pow(glow,26);
#endif
    float2 cloud_uv=d.xz/max(.08,d.y+.12);
#ifdef REMASTERED_LIGHT
    cloud_uv+=weather_seconds*float2(.009,.003);
#endif
    float clouds=grain(cloud_uv*2)*.60+grain(cloud_uv*5)*.29+grain(cloud_uv*14)*.11;
#ifdef REMASTERED_LIGHT
    float deck=smoothstep(.34-.24*cloud_cover,.77-.34*cloud_cover,clouds)*saturate(cloud_cover*2);
    deck*=lerp(smoothstep(-.02,.13,d.y),1,smoothstep(.55,1,cloud_cover));
    float ceiling=smoothstep(.55,.88,cloud_cover);
    deck=lerp(deck,1,ceiling);
    float3 cloud_color=lerp(float3(.12,.17,.26),lerp(float3(.56,.62,.67),float3(.96,.96,.92),
                            smoothstep(.31,.69,clouds)),day);
    float3 underside=lerp(float3(.12,.15,.23),float3(.60,.65,.69),day)*(.78+.34*clouds);
    cloud_color=lerp(cloud_color,underside,ceiling);
    cloud_color*=lerp(float3(1,1,1),float3(1.13,.82,.62),golden_hour(sun.y));
    sky=lerp(sky,cloud_color*(1-.19*rain_strength),deck);
    sky=lerp(sky,saturate(sun_color(sun)+.20),smoothstep(.99940,.99975,glow)*(1-deck)*smoothstep(-.04,.012,sun.y));
#else
    sky=lerp(sky,float3(.95,.93,.84),smoothstep(.47,.74,clouds)*smoothstep(.01,.16,d.y)*.62);
    sky=lerp(sky,float3(1,.97,.80),smoothstep(.99940,.99975,glow));
#endif
    // Closed angular profiles keep the distant coast continuous through a full turn.
    float heading=atan2(d.z,d.x);
    float far_ridge=.027+.020*sin(3*heading+.6)+.016*sin(7*heading)+.008*sin(15*heading+.7);
    float near_ridge=.004+.019*sin(5*heading-.5)+.011*sin(11*heading)+.004*sin(24*heading);
    float edge=max(.0004,fwidth(d.y));
    sky=lerp(sky,float3(.50,.61,.59),smoothstep(-edge,edge,far_ridge-d.y));
    sky=lerp(sky,float3(.33,.47,.43),smoothstep(-edge,edge,near_ridge-d.y));
    float2 water_uv=d.xz/max(.012,-d.y);
    float ripples=grain(water_uv*float2(5,19));
    float3 sea=lerp(float3(.32,.55,.57),float3(.48,.67,.64),ripples);
#ifdef REMASTERED_LIGHT
    // Distant ridges and water share the ambient light at the horizon.
    float land=smoothstep(-edge,edge,far_ridge-d.y);
    float3 coast_light=lerp(float3(.20,.25,.39),float3(1,1,1),day);
    sky*=lerp(float3(1,1,1),coast_light,land);
    sea*=coast_light;
    float3 coast=lerp(sky,sea,smoothstep(.021,.024,-d.y));
    float sky_path=lerp(250,28,smoothstep(-.03,.65,d.y));
    return air_perspective(coast,sky_path,sun);
#else
    return lerp(sky,sea,smoothstep(.021,.024,-d.y));
#endif
}
#endif
)"
