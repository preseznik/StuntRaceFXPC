void draw_driving_hud(Renderer& renderer,srf::GridCountdownArt& art,std::span<std::uint32_t> pixels,
    const srf::NativeRace& race,unsigned spares,bool show) {
    if(!show && !race.countdown_active() && !controls.paused && !race.ended()){renderer.clear_bonus_overlay();return;}
    art.driving(pixels,race,controls.paused,show);
    srf::GridCountdownFrame frame{};
    frame.spares=spares;
    renderer.set_bonus_overlay(pixels,15,1,40,show?art.dashboard(frame,race):std::span<const std::uint32_t>{},0,224,race.countdown_active());
}
