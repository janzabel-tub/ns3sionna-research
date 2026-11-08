function report = validate_he_error_matlab(binaryPath,awgnBinary,outputDirectory,trials)
%VALIDATE_HE_ERROR_MATLAB Independent multi-stream uncoded BER and coded PER.
% No fitted offset/beta. Native AWGN curves choose test points before any
% MATLAB packet measurement. Report implementation and model accuracy apart.
arguments
    binaryPath (1,1) string
    awgnBinary (1,1) string
    outputDirectory (1,1) string
    trials (1,1) double {mustBeInteger,mustBePositive} = 600
end
assert(~isfolder(outputDirectory),'Choose a fresh output directory');
mkdir(outputDirectory); rng(20261029,'twister');
mathCases = makeMathCases();
packetCases = makePacketCases(awgnBinary,outputDirectory);
allCases = [mathCases,packetCases];
writeFixtures(fullfile(outputDirectory,'fixtures.txt'),allCases);
command = "env -u LD_LIBRARY_PATH -u LD_PRELOAD "+quote(binaryPath)+" "+ ...
    quote(fullfile(outputDirectory,'fixtures.txt'))+" "+quote(fullfile(outputDirectory,'native.json'));
[status,log] = system(command); assert(status==0,'Native exporter failed: %s',log);
native = jsondecode(fileread(fullfile(outputDirectory,'native.json')));
assert(numel(native.cases)==numel(allCases));
mathReport = checkMapping(allCases,native.cases);
for k=1:numel(packetCases)
    pair=packetCases{k}.pair;
    if pair>0
        a=native.cases(numel(mathCases)+k); b=native.cases(numel(mathCases)+pair);
        assert(abs(a.effective-b.effective)<1e-8+1e-8*b.effective);
        assert(abs(a.per-b.per)<1e-12,'Matched PER cases must have identical predictions');
    end
end
writeJson(fullfile(outputDirectory,'mapping_report.json'),mathReport);
fprintf('Mapping checks PASSED: %d cases; max BER error %.6g\n', ...
    numel(allCases),mathReport.max_ber_error);

% Determine all tests and their family-wise confidence before sampling.
comparisons = numel(packetCases)+sum(cellfun(@(c) c.pair>0,packetCases));
alpha = 0.01/comparisons; z = sqrt(2)*erfcinv(alpha);
measurements = struct('id',{},'mcs',{},'ldpc',{},'nss',{},'apep_bytes',{}, ...
    'psdu_bytes',{},'trials',{},'errors',{},'measured_per',{},'predicted_per',{}, ...
    'wilson_lower',{},'wilson_upper',{},'table_prediction_inside_interval',{}, ...
    'mean_uncoded_ber',{},'effective_sinr',{},'sinr_min',{},'sinr_max',{}, ...
    'raw_ber',{},'expected_raw_ber',{},'raw_bit_errors',{},'raw_bit_count',{}, ...
    'clean_packet_errors',{},'pair',{});
for k = 1:numel(packetCases)
    c = packetCases{k}; n = native.cases(numel(mathCases)+k);
    [errors,rawBer,cleanErrors,rawErrors,rawCount] = measurePackets(c,trials,k);
    [lo,hi] = wilson(errors,trials,z);
    measurements(end+1) = struct('id',c.id,'mcs',c.mcs,'ldpc',c.ldpc, ...
        'nss',c.nss,'apep_bytes',c.apep,'psdu_bytes',c.bytes,'trials',trials, ...
        'errors',errors,'measured_per',errors/trials,'predicted_per',n.per, ...
        'wilson_lower',lo,'wilson_upper',hi, ...
        'table_prediction_inside_interval',n.per>=lo && n.per<=hi, ...
        'mean_uncoded_ber',n.mean_ber,'effective_sinr',n.effective, ...
        'sinr_min',min(c.gamma,[],'all'),'sinr_max',max(c.gamma,[],'all'), ...
        'raw_ber',rawBer,'expected_raw_ber', ...
        mean(he_gray_ber_reference(c.gamma,c.mcs),1), ...
        'raw_bit_errors',rawErrors,'raw_bit_count',rawCount, ...
        'clean_packet_errors',cleanErrors,'pair',c.pair); %#ok<AGROW>
    writeJson(fullfile(outputDirectory,'packet_measurements.json'),measurements);
    fprintf('PER %d/%d %s: %d/%d measured=%.5f predicted=%.5f interval=[%.5f,%.5f]\n', ...
        k,numel(packetCases),c.id,errors,trials,errors/trials,n.per,lo,hi);
end
pairs = struct('id',{},'flat_id',{},'per_difference',{},'difference_lower',{}, ...
    'difference_upper',{},'same_mean_ber_compatible',{});
for k = 1:numel(packetCases)
    if packetCases{k}.pair==0, continue; end
    a = measurements(k); b = measurements(packetCases{k}.pair);
    difference = a.measured_per-b.measured_per;
    % Newcombe two-sample interval from independent Wilson score intervals.
    lower = difference-sqrt((a.measured_per-a.wilson_lower)^2+ ...
        (b.wilson_upper-b.measured_per)^2);
    upper = difference+sqrt((a.wilson_upper-a.measured_per)^2+ ...
        (b.measured_per-b.wilson_lower)^2);
    pairs(end+1) = struct('id',a.id,'flat_id',b.id,'per_difference',difference, ...
        'difference_lower',lower,'difference_upper',upper, ...
        'same_mean_ber_compatible',lower<=0 && upper>=0); %#ok<AGROW>
end
tableFailures = sum(~[measurements.table_prediction_inside_interval]);
mappingFailures = sum(~[pairs.same_mean_ber_compatible]);
report = struct('schema_version',1,'execution_status','completed', ...
    'mapping_implementation_status','passed','coded_per_accuracy_status','passed', ...
    'matlab_version',version,'matlab_release',version('-release'),'rng_seed',20261029, ...
    'toolboxes',ver,'ldpc_decoder','bp; MaximumLDPCIterationCount=12', ...
    'scope','Ideal-CSI HE data, BCC/LDPC decoding, FFT-domain diagonal AWGN; no header detection', ...
    'native_mapping',mathReport,'trials_per_case',trials,'packet_cases',numel(packetCases), ...
    'familywise_alpha',0.01,'comparison_count',comparisons,'per_comparison_alpha',alpha, ...
    'confidence_z',z,'native_per_failures',tableFailures,'matched_awgn_failures',mappingFailures, ...
    'measurements',measurements,'matched_awgn_pairs',pairs);
if tableFailures || mappingFailures, report.coded_per_accuracy_status='failed'; end
writeJson(fullfile(outputDirectory,'report.json'),report);
fprintf('Completed: implementation passed; table PER failures %d/%d; matched-AWGN failures %d/%d\n', ...
    tableFailures,numel(measurements),mappingFailures,numel(pairs));
end

function cases = makeMathCases()
cases = {};
for mcs = 0:11
    for gamma = [0,logspace(-8,4,49)]
        c = base(mcs,1,1); c.gamma = gamma;
        c.id = sprintf('scalar_mcs%d_g%.9g',mcs,gamma); cases{end+1}=c; %#ok<AGROW>
    end
    for nss = [1,2,3,4,8]
        for pattern = 1:6
            c = base(mcs,nss,7); c.id=sprintf('streams_mcs%d_nss%d_p%d',mcs,nss,pattern);
            switch pattern
                case 1, c.gamma(:)=3;
                case 2, c.gamma=repmat(logspace(-3,3,nss),7,1);
                case 3, c.gamma=repmat(logspace(-3,3,7)',1,nss);
                case 4, c.gamma=10.^(-3+6*rand(7,nss));
                case 5, c.gamma=10.^(-2+4*rand(7,nss)); c.selected=[1,3,5]; c.gamma([2,4,6,7],:)=1e3;
                case 6, c.gamma=zeros(7,nss); c.gamma(:,end)=20;
            end
            cases{end+1}=c; %#ok<AGROW>
        end
    end
end
end

function cases = makePacketCases(awgnBinary,directory)
cases={}; profiles=[0,0;1,0;3,0;5,0;8,0;0,1;10,1];
for p = 1:size(profiles,1)
    for nss = [1,2,4]
        c=packetBase(profiles(p,1),profiles(p,2),nss,256);
        c.id=sprintf('per_mcs%d_ldpc%d_nss%d_flat',c.mcs,c.ldpc,nss);
        target=chooseSnr(awgnBinary,c,directory); c.gamma(:)=target;
        cases{end+1}=c; flatIndex=numel(cases); %#ok<AGROW>
        if nss==2 || (nss==4 && ismember(p,[1,3,7]))
            [weak,strong]=equalBerPair(target,c.mcs);
            c.gamma(:,1:nss/2)=weak; c.gamma(:,nss/2+1:end)=strong;
            c.id=strrep(c.id,'_flat','_unequal_streams'); c.pair=flatIndex;
            cases{end+1}=c; %#ok<AGROW>
            if nss==2
                c.gamma(1:117,:)=weak; c.gamma(118:234,:)=strong;
                c.id=strrep(c.id,'_unequal_streams','_frequency_selective');
                cases{end+1}=c; %#ok<AGROW>
            end
        end
    end
end
for apep=[32,1200]
    c=packetBase(0,0,2,apep); c.id=sprintf('per_length%d_nss2_flat',apep);
    target=chooseSnr(awgnBinary,c,directory); c.gamma(:)=target;
    cases{end+1}=c; flatIndex=numel(cases); %#ok<AGROW>
    [weak,strong]=equalBerPair(target,c.mcs);
    c.gamma(:,1)=weak; c.gamma(:,2)=strong; c.pair=flatIndex;
    c.id=strrep(c.id,'_flat','_unequal_streams'); cases{end+1}=c; %#ok<AGROW>
end
end

function c=base(mcs,nss,tones)
c=struct('id','','mcs',mcs,'nss',nss,'bytes',256,'ldpc',mcs>=10, ...
    'gamma',zeros(tones,nss),'selected',[],'pair',0,'apep',256);
end
function c=packetBase(mcs,ldpc,nss,apep)
c=base(mcs,nss,234); c.ldpc=logical(ldpc); c.apep=apep;
cfg=configuration(c); c.bytes=getPSDULength(cfg);
end
function cfg=configuration(c)
coding='BCC'; if c.ldpc, coding='LDPC'; end
cfg=wlanHESUConfig(ChannelBandwidth='CBW20',MCS=c.mcs,APEPLength=c.apep, ...
    ChannelCoding=coding,NumSpaceTimeStreams=c.nss,NumTransmitAntennas=c.nss, ...
    GuardInterval=0.8,SpatialMapping='Direct');
end
function target=chooseSnr(binary,c,directory)
command="env -u LD_LIBRARY_PATH -u LD_PRELOAD "+quote(binary)+ ...
    sprintf(' --mcs=%d --nss=%d --numRxAntennas=%d --packetSizeBytes=%d --ldpc=%d --snrStartDb=-8 --snrStopDb=40 --snrStepDb=0.1', ...
    c.mcs,c.nss,c.nss,c.bytes,c.ldpc);
[status,text]=system(command); assert(status==0,'AWGN curve failed');
path=fullfile(directory,c.id+"_native_awgn.csv"); writeText(path,text);
table=readmatrix(path,NumHeaderLines=1); [~,k]=min(abs(table(:,end)-0.5));
target=10^(table(k,end-1)/10);
end
function [weak,strong]=equalBerPair(target,mcs)
strong=target*10^(12/10);
weakBer=2*he_gray_ber_reference(target,mcs)-he_gray_ber_reference(strong,mcs);
assert(weakBer>0 && weakBer<0.5);
weak=invertBer(weakBer,mcs,0,target);
end
function gamma=invertBer(ber,mcs,low,high)
if low==high || ber==0, gamma=low; return; end
f=@(t) he_gray_ber_reference(expm1(t),mcs)-ber;
a=log1p(low); b=log1p(high);
if f(a)==0, gamma=low; elseif f(b)==0, gamma=high;
else, gamma=expm1(fzero(f,[a,b],optimset('TolX',1e-13))); end
end
function writeFixtures(path,cases)
f=fopen(path,'w'); assert(f>=0); cleanup=onCleanup(@()fclose(f));
fprintf(f,'%d\n',numel(cases));
for k=1:numel(cases)
    c=cases{k};
    fprintf(f,'%s %d %d %d %d %d %d\n',c.id,c.mcs,c.nss,size(c.gamma,1),c.bytes,c.ldpc,numel(c.selected));
    fprintf(f,'%d ',c.selected-1); fprintf(f,'\n');
    fprintf(f,'%.17g ',c.gamma.'); fprintf(f,'\n');
end
end
function r=checkMapping(cases,native)
maxBer=0; maxEffective=0; maxPost=0; comparisons=0; details=struct('id',{},'mean_ber',{},'effective',{});
for k=1:numel(cases)
    c=cases{k}; n=native(k); assert(strcmp(n.id,c.id));
    expected=he_gray_ber_reference(c.gamma,c.mcs); actual=reshape(n.sample_ber,c.nss,[]).';
    e=max(abs(actual-expected),[],'all'); assert(e<2e-13,'BER mismatch %s',c.id); maxBer=max(maxBer,e);
    post=reshape(n.post_sinr,c.nss,[]).';
    e=max(abs(post-c.gamma),[],'all'); maxPost=max(maxPost,e);
    assert(all(abs(post-c.gamma)<=1e-9+1e-9*abs(c.gamma),'all'),'Post-SINR mismatch');
    selected=c.selected; if isempty(selected), selected=1:size(c.gamma,1); end
    values=c.gamma(selected,:); meanBer=mean(expected(selected,:),'all');
    effective=invertBer(meanBer,c.mcs,min(values,[],'all'),max(values,[],'all'));
    assert(abs(n.mean_ber-meanBer)<2e-13,'Mean BER mismatch');
    assert(abs(n.effective-effective)<=1e-8+1e-8*effective,'Effective mismatch %s',c.id);
    assert(abs(n.dl_effective-effective)<=1e-8+1e-8*effective,'DL stream pooling mismatch');
    other=values*0.1; otherMean=mean(he_gray_ber_reference(other,c.mcs),'all');
    otherEffective=invertBer(otherMean,c.mcs,min(other,[],'all'),max(other,[],'all'));
    assert(abs(n.dl2_effective-otherEffective)<=1e-8+1e-8*otherEffective,'DL user isolation mismatch');
    assert(abs(n.per-n.dl_per)<1e-12,'SU/DL PER mismatch');
    assert(isfinite(n.per) && n.per>=0 && n.per<=1);
    maxEffective=max(maxEffective,abs(n.effective-effective));
    comparisons=comparisons+2*numel(expected)+6;
    details(end+1)=struct('id',c.id,'mean_ber',meanBer,'effective',effective); %#ok<AGROW>
end
r=struct('status','passed','case_count',numel(cases),'scalar_comparisons',comparisons, ...
    'max_ber_error',maxBer,'max_effective_sinr_error',maxEffective,'max_post_sinr_error',maxPost, ...
    'ber_reference','qammod Gray labels and Gaussian decision-region integration', ...
    'cases',details);
end

function [errors,rawBer,cleanErrors,rawErrors,rawCount]=measurePackets(c,trials,index)
rng(20261029+index,'twister'); cfg=configuration(c); assert(getPSDULength(cfg)==c.bytes);
fields=wlanFieldIndices(cfg); ofdm=wlanHEOFDMInfo('HE-Data',cfg); assert(numel(ofdm.DataIndices)==234);
testBits=randi([0,1],c.bytes*8,1,'int8');
wave=wlanWaveformGenerator(testBits,cfg,WindowTransitionTime=0);
ltf=wlanHEDemodulate(wave(fields.HELTF(1):fields.HELTF(2),:),'HE-LTF',cfg);
h=wlanHELTFChannelEstimate(ltf,cfg); h=h(ofdm.DataIndices,:,:);
gain=zeros(234,c.nss); targetH=h;
for s=1:c.nss
    assert(max(abs(h(:,s,s)))>0);
    gain(:,s)=sqrt(c.gamma(:,s))./abs(h(:,s,s));
end
for rx=1:c.nss, targetH(:,:,rx)=h(:,:,rx).*gain(:,rx); end
errors=0; rawErrors=zeros(1,c.nss); rawCount=zeros(1,c.nss); cleanErrors=0;
sizes=[2 4 4 16 16 64 64 64 256 256 1024 1024]; M=sizes(c.mcs+1);
for packet=1:trials
    bits=randi([0,1],c.bytes*8,1,'int8');
    wave=wlanWaveformGenerator(bits,cfg,WindowTransitionTime=0,ScramblerInitialization=randi([1,127]));
    symbols=wlanHEDemodulate(wave(fields.HEData(1):fields.HEData(2),:),'HE-Data',cfg);
    symbols=symbols(ofdm.DataIndices,:,:);
    noisy=zeros(size(symbols));
    for s=1:c.nss
        clean=symbols(:,:,s).*gain(:,s);
        noisy(:,:,s)=clean+(randn(size(clean))+1i*randn(size(clean)))/sqrt(2);
        sent=symbols(:,:,s)./h(:,s,s);
        received=noisy(:,:,s)./targetH(:,s,s);
        if M==2
            rawErrors(s)=rawErrors(s)+sum((real(sent)<0)~=(real(received)<0),'all');
        else
            a=qamdemod(sent(:),M,'gray',UnitAveragePower=true,OutputType='bit');
            b=qamdemod(received(:),M,'gray',UnitAveragePower=true,OutputType='bit');
            rawErrors(s)=rawErrors(s)+sum(a~=b);
        end
        rawCount(s)=rawCount(s)+numel(sent)*log2(M);
    end
    [eq,csi]=wlanHEEqualize(noisy,targetH,1,cfg,'HE-Data');
    decoded=wlanHEDataBitRecover(eq,1,csi,cfg,LDPCDecodingMethod='bp',MaximumLDPCIterationCount=12);
    errors=errors+~isequal(bits,decoded);
    if packet==1
        cleanSymbols=symbols;
        for s=1:c.nss, cleanSymbols(:,:,s)=symbols(:,:,s).*gain(:,s); end
        [eqClean,csiClean]=wlanHEEqualize(cleanSymbols,targetH,1e-12,cfg,'HE-Data');
        cleanDecoded=wlanHEDataBitRecover(eqClean,1e-12,csiClean,cfg);
        cleanErrors=~isequal(bits,cleanDecoded); assert(cleanErrors==0,'Noiseless receiver failed');
    end
end
rawBer=rawErrors./rawCount;
end
function [low,high]=wilson(errors,trials,z)
p=errors/trials; center=(p+z^2/(2*trials))/(1+z^2/trials);
half=z/(1+z^2/trials)*sqrt(p*(1-p)/trials+z^2/(4*trials^2));
low=max(0,center-half); high=min(1,center+half);
end
function q=quote(value)
q="'"+replace(string(value),"'","'"+char(34)+"'"+char(34)+"'")+"'";
end
function writeJson(path,value), writeText(path,jsonencode(value,PrettyPrint=true)); end
function writeText(path,value)
f=fopen(path,'w'); assert(f>=0); cleanup=onCleanup(@()fclose(f)); fprintf(f,'%s\n',value);
end
