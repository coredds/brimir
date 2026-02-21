#include <brimir/hw/scsp/scsp_slot.hpp>

namespace brimir::scsp {

Slot::Slot() {
    Reset();
}

void Slot::Reset() {
    startAddress = 0;
    loopStartAddress = 0;
    loopEndAddress = 0;
    pcm8Bit = false;
    keyOnBit = false;
    loopControl = LoopControl::Off;
    sampleXOR = 0x0000;
    soundSource = SoundSource::SoundRAM;

    attackRate = 0;
    decay1Rate = 0;
    decay2Rate = 0;
    releaseRate = 0;

    decayLevel = 0;

    keyRateScaling = 0;

    egHold = false;
    egBypass = false;

    loopStartLink = false;

    modLevel = 0;
    modXSelect = 0;
    modYSelect = 0;
    stackWriteInhibit = false;

    totalLevel = 0;
    soundDirect = false;

    octave = 0;
    freqNumSwitch = 0x400u;
    maskMode = false;

    lfoReset = false;
    lfofRaw = 0;
    lfoStepInterval = s_lfoStepTbl[0];
    ampLFOSens = 0;
    ampLFOWaveform = Waveform::Saw;
    pitchLFOWaveform = Waveform::Saw;

    inputMixingLevel = 0;
    inputSelect = 0;
    directSendLevel = 0;
    directPan = 0;
    effectSendLevel = 0;
    effectPan = 0;

    extraBits0C = 0;
    extraBits14 = 0;

    active = false;

    egState = EGState::Release;
    currEGRate = releaseRate;

    egLevel = 0x3FF;

    currSample = 0;
    currPhase = 0;
    nextPhase = 0;
    modXSample = 0;
    modYSample = 0;
    modulation = 0;
    reverse = false;
    crossedLoopStart = false;

    lfoCycles = 0;
    lfoStep = 0;

    alfoOutput = 0;

    sample1 = 0;
    sample2 = 0;

    output = 0;

    UpdateMask();
}

void Slot::SaveState(state::SCSPSlotState &state) const {
    state.SA = startAddress;
    state.LSA = loopStartAddress;
    state.LEA = loopEndAddress;
    state.PCM8B = pcm8Bit;
    state.KYONB = keyOnBit;
    state.SBCTL = sampleXOR;

    switch (loopControl) {
    default: [[fallthrough]];
    case LoopControl::Off: state.LPCTL = state::SCSPSlotState::LoopControl::Off; break;
    case LoopControl::Normal: state.LPCTL = state::SCSPSlotState::LoopControl::Normal; break;
    case LoopControl::Reverse: state.LPCTL = state::SCSPSlotState::LoopControl::Reverse; break;
    case LoopControl::Alternate: state.LPCTL = state::SCSPSlotState::LoopControl::Alternate; break;
    }

    switch (soundSource) {
    default: [[fallthrough]];
    case SoundSource::SoundRAM: state.SSCTL = state::SCSPSlotState::SoundSource::SoundRAM; break;
    case SoundSource::Noise: state.SSCTL = state::SCSPSlotState::SoundSource::Noise; break;
    case SoundSource::Silence: state.SSCTL = state::SCSPSlotState::SoundSource::Silence; break;
    case SoundSource::Unknown: state.SSCTL = state::SCSPSlotState::SoundSource::Unknown; break;
    }

    state.AR = attackRate;
    state.D1R = decay1Rate;
    state.D2R = decay2Rate;
    state.RR = releaseRate;
    state.DL = decayLevel;

    state.KRS = keyRateScaling;
    state.EGHOLD = egHold;
    state.LPSLNK = loopStartLink;
    state.EGBYPASS = egBypass;

    state.MDL = modLevel;
    state.MDXSL = modXSelect;
    state.MDYSL = modYSelect;
    state.STWINH = stackWriteInhibit;

    state.TL = totalLevel;
    state.SDIR = soundDirect;

    state.OCT = octave;
    state.FNS = freqNumSwitch;
    state.MM = maskMode;

    auto castWaveform = [](Waveform waveform) {
        switch (waveform) {
        default: [[fallthrough]];
        case Waveform::Saw: return state::SCSPSlotState::Waveform::Saw;
        case Waveform::Square: return state::SCSPSlotState::Waveform::Square;
        case Waveform::Triangle: return state::SCSPSlotState::Waveform::Triangle;
        case Waveform::Noise: return state::SCSPSlotState::Waveform::Noise;
        }
    };

    state.LFORE = lfoReset;
    state.LFOF = lfofRaw;
    state.ALFOS = ampLFOSens;
    state.PLFOS = pitchLFOSens;
    state.ALFOWS = castWaveform(ampLFOWaveform);
    state.PLFOWS = castWaveform(pitchLFOWaveform);

    state.IMXL = inputMixingLevel;
    state.ISEL = inputSelect;
    state.DISDL = directSendLevel;
    state.DIPAN = directPan;

    state.EFSDL = effectSendLevel;
    state.EFPAN = effectPan;

    state.extra0C = extraBits0C;
    state.extra14 = extraBits14;

    state.active = active;

    switch (egState) {
    default: [[fallthrough]];
    case EGState::Attack: state.egState = state::SCSPSlotState::EGState::Attack; break;
    case EGState::Decay1: state.egState = state::SCSPSlotState::EGState::Decay1; break;
    case EGState::Decay2: state.egState = state::SCSPSlotState::EGState::Decay2; break;
    case EGState::Release: state.egState = state::SCSPSlotState::EGState::Release; break;
    }

    state.egLevel = egLevel;
    state.currEGLevel = currEGLevel;
    state.egAttackBug = egAttackBug;

    state.currSample = currSample;
    state.currPhase = currPhase;
    state.nextPhase = nextPhase;
    state.modulation = modulation;
    state.reverse = reverse;
    state.crossedLoopStart = crossedLoopStart;

    state.lfoCycles = lfoCycles;
    state.lfoStep = lfoStep;

    state.alfoOutput = alfoOutput;

    state.sample1 = sample1;
    state.sample2 = sample2;
    state.output = output;

    state.finalLevel = finalLevel;
}

bool Slot::ValidateState(const state::SCSPSlotState &state) const {
    switch (state.LPCTL) {
    case state::SCSPSlotState::LoopControl::Off: break;
    case state::SCSPSlotState::LoopControl::Normal: break;
    case state::SCSPSlotState::LoopControl::Reverse: break;
    case state::SCSPSlotState::LoopControl::Alternate: break;
    default: return false;
    }

    switch (state.SSCTL) {
    case state::SCSPSlotState::SoundSource::SoundRAM: break;
    case state::SCSPSlotState::SoundSource::Noise: break;
    case state::SCSPSlotState::SoundSource::Silence: break;
    case state::SCSPSlotState::SoundSource::Unknown: break;
    default: return false;
    }

    auto checkWaveform = [](state::SCSPSlotState::Waveform waveform) {
        switch (waveform) {
        case state::SCSPSlotState::Waveform::Saw: return true;
        case state::SCSPSlotState::Waveform::Square: return true;
        case state::SCSPSlotState::Waveform::Triangle: return true;
        case state::SCSPSlotState::Waveform::Noise: return true;
        default: return false;
        }
    };
    if (!checkWaveform(state.ALFOWS)) {
        return false;
    }
    if (!checkWaveform(state.PLFOWS)) {
        return false;
    }

    switch (state.egState) {
    case state::SCSPSlotState::EGState::Attack: break;
    case state::SCSPSlotState::EGState::Decay1: break;
    case state::SCSPSlotState::EGState::Decay2: break;
    case state::SCSPSlotState::EGState::Release: break;
    default: return false;
    }

    return true;
}

void Slot::LoadState(const state::SCSPSlotState &state) {
    startAddress = state.SA & 0xFFFFF;
    loopStartAddress = state.LSA & 0xFFFF;
    loopEndAddress = state.LEA & 0xFFFF;
    pcm8Bit = state.PCM8B;
    keyOnBit = state.KYONB;
    sampleXOR = state.SBCTL;

    switch (state.LPCTL) {
    default: [[fallthrough]];
    case state::SCSPSlotState::LoopControl::Off: loopControl = LoopControl::Off; break;
    case state::SCSPSlotState::LoopControl::Normal: loopControl = LoopControl::Normal; break;
    case state::SCSPSlotState::LoopControl::Reverse: loopControl = LoopControl::Reverse; break;
    case state::SCSPSlotState::LoopControl::Alternate: loopControl = LoopControl::Alternate; break;
    }

    switch (state.SSCTL) {
    default: [[fallthrough]];
    case state::SCSPSlotState::SoundSource::SoundRAM: soundSource = SoundSource::SoundRAM; break;
    case state::SCSPSlotState::SoundSource::Noise: soundSource = SoundSource::Noise; break;
    case state::SCSPSlotState::SoundSource::Silence: soundSource = SoundSource::Silence; break;
    case state::SCSPSlotState::SoundSource::Unknown: soundSource = SoundSource::Unknown; break;
    }

    attackRate = state.AR & 0x1F;
    decay1Rate = state.D1R & 0x1F;
    decay2Rate = state.D2R & 0x1F;
    releaseRate = state.RR & 0x1F;
    decayLevel = state.DL & 0x1F;

    keyRateScaling = state.KRS & 0xF;
    egHold = state.EGHOLD;
    loopStartLink = state.LPSLNK;
    egBypass = state.EGBYPASS;

    modLevel = state.MDL & 0xF;
    modXSelect = state.MDXSL & 0x3F;
    modYSelect = state.MDYSL & 0x3F;
    stackWriteInhibit = state.STWINH;

    totalLevel = state.TL;
    soundDirect = state.SDIR;

    octave = state.OCT & 0xF;
    freqNumSwitch = state.FNS & 0x7FF;
    maskMode = state.MM;

    auto castWaveform = [](state::SCSPSlotState::Waveform waveform) {
        switch (waveform) {
        default: [[fallthrough]];
        case state::SCSPSlotState::Waveform::Saw: return Waveform::Saw;
        case state::SCSPSlotState::Waveform::Square: return Waveform::Square;
        case state::SCSPSlotState::Waveform::Triangle: return Waveform::Triangle;
        case state::SCSPSlotState::Waveform::Noise: return Waveform::Noise;
        }
    };

    lfoReset = state.LFORE;
    lfofRaw = state.LFOF & 0x1F;
    lfoStepInterval = s_lfoStepTbl[lfofRaw];
    ampLFOSens = state.ALFOS & 0x7;
    pitchLFOSens = state.PLFOS & 0x7;
    ampLFOWaveform = castWaveform(state.ALFOWS);
    pitchLFOWaveform = castWaveform(state.PLFOWS);

    inputMixingLevel = state.IMXL & 0x7;
    inputSelect = state.ISEL & 0xF;
    directSendLevel = state.DISDL & 0x7;
    directPan = state.DIPAN & 0x1F;

    effectSendLevel = state.EFSDL & 0x7;
    effectPan = state.EFPAN & 0x1F;

    extraBits0C = state.extra0C;
    extraBits14 = state.extra14;

    active = state.active;

    switch (state.egState) {
    default: [[fallthrough]];
    case state::SCSPSlotState::EGState::Attack: egState = EGState::Attack; break;
    case state::SCSPSlotState::EGState::Decay1: egState = EGState::Decay1; break;
    case state::SCSPSlotState::EGState::Decay2: egState = EGState::Decay2; break;
    case state::SCSPSlotState::EGState::Release: egState = EGState::Release; break;
    }

    switch (state.egState) {
    default: [[fallthrough]];
    case state::SCSPSlotState::EGState::Attack: currEGRate = attackRate; break;
    case state::SCSPSlotState::EGState::Decay1: currEGRate = decay1Rate; break;
    case state::SCSPSlotState::EGState::Decay2: currEGRate = decay2Rate; break;
    case state::SCSPSlotState::EGState::Release: currEGRate = releaseRate; break;
    }

    egLevel = state.egLevel & 0x3FF;
    currEGLevel = state.currEGLevel & 0x3FF;
    egAttackBug = state.egAttackBug;

    currSample = state.currSample;
    currPhase = state.currPhase;
    nextPhase = state.nextPhase;
    modulation = state.modulation;
    reverse = state.reverse;
    crossedLoopStart = state.crossedLoopStart;

    lfoCycles = state.lfoCycles;
    lfoStep = state.lfoStep;

    alfoOutput = state.alfoOutput;

    sample1 = state.sample1;
    sample2 = state.sample2;
    output = state.output;

    finalLevel = state.finalLevel;

    UpdateMask();
}

} // namespace brimir::scsp
