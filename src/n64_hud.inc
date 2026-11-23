// Cartridge scoreboard lettering at the scene raster size.
class N64RaceHud {
    std::vector<std::uint8_t> letters;
    std::array<std::uint32_t,16> inks;
    std::vector<std::uint32_t> pixels=std::vector<std::uint32_t>(208*128);
    std::array<std::string,7> previous{};
    bool visible=false;
public:
    explicit N64RaceHud(const srf::Rom& rom):letters(srf::unpack_rom_graphics(rom,0x18c770)),
        inks(srf::bonus_hud_palette(rom,srf::Rom::lorom_offset(0x1f8200)+2*(128+16))) {}

    void update(Renderer& renderer,const srf::NativeRace& race,bool show) {
        if(!show) {
            if(visible)renderer.clear_bonus_overlay();
            visible=false;return;
        }
        const auto player=race.player_index();
        const auto& body=race.racers()[player].body;
        std::array<std::string,7> rows{
            "LAP "+std::to_string(std::min(3u,unsigned(body.word(0x7a))+1))+" OF 3",
            "POS "+std::to_string(race.standings()[player]),
            "TIME "+std::to_string(race.remaining_seconds()),
            "BOOST "+std::to_string(body.word(0x6a)),
            race.completion().finished?"FINISH  R RESTART":race.ended()?"R RESTART":
            controls.paused?"PAUSED":race.countdown_active()?"READY":"",
            "SPD "+std::to_string(std::abs(int(std::int16_t(body.word(0x4a))))),
            "HP "+std::to_string(std::max(0,512-int(body.word(0x82))))
        };
        if(visible && previous==rows)return;
        previous=rows;visible=true;
        std::fill(pixels.begin(),pixels.end(),0);
        std::array<std::uint32_t,16> shadow;shadow.fill(0xff242428);
        auto text=[&](int x,int y,const std::string& words) {
            for(char c:words) {
                if(c!=' ') {
                    unsigned glyph=c<='9'?c-'0':10+c-'A';
                    srf::draw_bonus_hud_tile(pixels,208,x+1,y+1,letters,glyph,shadow);
                    srf::draw_bonus_hud_tile(pixels,208,x,y,letters,glyph,inks);
                }
                x+=8;
            }
        };
        text(4,4,rows[0]);text(4,14,rows[1]);text(204-8*int(rows[2].size()),4,rows[2]);
        text(4,116,rows[3]);text((208-int(rows[4].size())*8)/2,51,rows[4]);
        text(204-8*int(rows[5].size()),116,rows[5]);text(4,106,rows[6]);
        renderer.set_bonus_overlay(pixels,15,1,26,{});
    }
};
