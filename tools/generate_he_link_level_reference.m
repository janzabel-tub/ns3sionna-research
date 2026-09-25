function generate_he_link_level_reference(outputPath, trials)
%GENERATE_HE_LINK_LEVEL_REFERENCE Bit-level HE SU PER reference for Phase 5.1.
% Uses MATLAB WLAN Toolbox R2026a. Each row holds one fixed channel and SNR;
% independent PSDUs and noise are used for every packet. Receiver timing and
% CSI are ideal, while noise estimation, MMSE equalization and FEC decoding
% use the standard WLAN Toolbox functions. All SINRs are measured from the
% noise-free and noisy demodulated HE-Data tones before equalization.
%
% Example:
%   generate_he_link_level_reference('/tmp/he_mcs0_bcc_32.json',1000)

arguments
    outputPath (1,1) string
    trials (1,1) double {mustBeInteger,mustBePositive} = 1000
end

cfg = wlanHESUConfig(ChannelBandwidth='CBW20',MCS=0, ...
    APEPLength=32,ChannelCoding='BCC',NumSpaceTimeStreams=1, ...
    NumTransmitAntennas=1,GuardInterval=0.8);
ofdm = wlanHEOFDMInfo('HE-Data',cfg);
fields = wlanFieldIndices(cfg);
dataTones = ofdm.DataIndices;

reference.source = ['Local MATLAB WLAN Toolbox HE SU bit-level simulation; ' ...
    'tools/generate_he_link_level_reference.m; based on ' ...
    'https://www.mathworks.com/help/wlan/ug/802-11ax-packet-error-rate-simulation-for-single-user-format.html'];
reference.source_version = version('-release');
reference.source_kind = 'link_level';
reference.phy = struct('mcs',0,'coding','BCC','packet_size_bytes',32, ...
    'nss',1,'num_rx_antennas',1,'channel_width_mhz',20, ...
    'guard_interval_ns',800);
reference.awgn = struct('snr_db',{},'split',{},'errors',{},'trials',{});
reference.selective = struct('case_id',{},'split',{},'sinr_linear',{}, ...
    'errors',{},'trials',{});

% Delays remain well inside the 64-sample HE guard interval. Tap energy is
% normalized so that different cases have the same wideband channel power.
impulseResponses = {
    1, ...
    [1; zeros(7,1); 0.5], ...
    [1; zeros(11,1); -0.7], ...
    [1; zeros(5,1); 0.8i] ...
};
caseNames = {'awgn','tap8_positive','tap12_negative','tap6_quadrature'};
splits = {'','train','train','validation'};
nominalSnrDb = {
    [-3 -2 -1 0 1], ...
    [-2 -1 0 1 2], ...
    [-2 -1 0 1 2], ...
    [-2 -1 0 1 2] ...
};

for caseIndex = 1:numel(impulseResponses)
    h = impulseResponses{caseIndex};
    h = h / norm(h);
    for snrIndex = 1:numel(nominalSnrDb{caseIndex})
        nominal = nominalSnrDb{caseIndex}(snrIndex);
        rng(5000 + caseIndex*100 + snrIndex,'twister');
        packetErrors = 0;
        signalEnergy = zeros(numel(dataTones),1);
        noiseEnergy = zeros(numel(dataTones),1);
        for packetIndex = 1:trials
            bits = randi([0 1],getPSDULength(cfg)*8,1);
            tx = wlanWaveformGenerator(bits,cfg);
            clean = filter(h,1,tx);
            rx = awgn(clean,nominal,'measured');

            % Use perfect receiver CSI, matching the ns-3 abstraction.
            ltf = wlanHEDemodulate(clean(fields.HELTF(1):fields.HELTF(2)), ...
                'HE-LTF',cfg);
            [channelEstimate,pilotEstimate] = wlanHELTFChannelEstimate(ltf,cfg);
            noisySymbols = wlanHEDemodulate( ...
                rx(fields.HEData(1):fields.HEData(2)),'HE-Data',cfg);
            cleanSymbols = wlanHEDemodulate( ...
                clean(fields.HEData(1):fields.HEData(2)),'HE-Data',cfg);

            signalEnergy = signalEnergy + sum(abs(cleanSymbols(dataTones,:)).^2,2);
            noiseEnergy = noiseEnergy + sum(abs( ...
                noisySymbols(dataTones,:)-cleanSymbols(dataTones,:)).^2,2);

            tracked = wlanHETrackPilotError(noisySymbols,channelEstimate,cfg,'HE-Data');
            noiseEstimate = wlanHEDataNoiseEstimate( ...
                tracked(ofdm.PilotIndices,:,:),pilotEstimate,cfg);
            [equalized,csi] = wlanHEEqualize( ...
                tracked(dataTones,:,:),channelEstimate(dataTones,:,:), ...
                noiseEstimate,cfg,'HE-Data');
            decoded = wlanHEDataBitRecover(equalized,noiseEstimate,csi,cfg);
            packetErrors = packetErrors + ~isequal(bits,decoded);
        end

        sinrLinear = signalEnergy ./ noiseEnergy;
        if any(~isfinite(sinrLinear)) || any(sinrLinear <= 0)
            error('Invalid measured per-tone SINR');
        end
        if caseIndex == 1
            split = 'train';
            if mod(snrIndex,2) == 0
                split = 'validation';
            end
            row = struct('snr_db',10*log10(sum(signalEnergy)/sum(noiseEnergy)), ...
                'split',split,'errors',packetErrors,'trials',trials);
            reference.awgn(end+1) = row;
        else
            row = struct('case_id',caseNames{caseIndex}, ...
                'split',splits{caseIndex}, ...
                'sinr_linear',reshape(sinrLinear,1,[]), ...
                'errors',packetErrors,'trials',trials);
            reference.selective(end+1) = row;
        end
        fprintf('%s nominalSNR=%g measuredSNR=%g errors=%d/%d\n', ...
            caseNames{caseIndex},nominal, ...
            10*log10(sum(signalEnergy)/sum(noiseEnergy)), ...
            packetErrors,trials);
    end
end

encoded = jsonencode(reference,PrettyPrint=true);
file = fopen(outputPath,'w');
if file < 0
    error('Could not open output file: %s',outputPath);
end
cleanup = onCleanup(@() fclose(file));
fprintf(file,'%s\n',encoded);
end
