function report = validate_mumimo_matlab(binaryPath, outputDirectory)
%VALIDATE_MUMIMO_MATLAB Independent MU-MIMO numerical cross-validation.
% Requires base MATLAB only. Does not validate WLAN waveforms, EESM or PER.
% MATLAB generates the inputs, invokes the production ns-3 kernels, and
% recomputes all results without importing ns-3's precoders or combiners.
% ZF uses QR rather than the C++ SVD; MMSE uses Cholesky and also checks SINR
% independently using the interference-only covariance identity.
arguments
    binaryPath (1,1) string
    outputDirectory (1,1) string
end
if ~isfile(binaryPath)
    error('mumimo:MissingBinary','Exporter not found: %s',binaryPath);
end
if isfolder(outputDirectory)
    error('mumimo:ExistingOutput','Choose a fresh output directory: %s',outputDirectory);
end
mkdir(outputDirectory);
rng(20261028,'twister');
cases = makeCases();
inputPath = fullfile(outputDirectory,'fixtures.txt');
nativePath = fullfile(outputDirectory,'native.json');
writeFixtures(inputPath,cases);
% MATLAB's bundled libstdc++ is older than the host compiler's runtime.
% Clear only the child process's loader overrides; the binary carries RUNPATH.
[status,log] = system("env -u LD_LIBRARY_PATH -u LD_PRELOAD " + ...
    shellQuote(binaryPath) + " " + shellQuote(inputPath) + ...
    " " + shellQuote(nativePath));
writeText(fullfile(outputDirectory,'native.log'),log);
if status ~= 0
    error('mumimo:NativeFailed','Native exporter failed: %s',log);
end
native = jsondecode(fileread(nativePath));
assert(native.schema_version == 1 && numel(native.cases) == numel(cases));
absoluteTolerance = 1e-10;
relativeTolerance = 1e-9;
details = struct('id',{},'direction',{},'method',{},'status',{}, ...
    'rows',{},'comparisons',{},'max_absolute_error',{},'max_scaled_error',{});
references = struct('id',{},'status',{},'precoder',{},'rows',{});
maxError = 0; maxScaled = 0; comparisons = 0; rowCount = 0; rejected = 0;
sinrMaxError = 0; sinrMaxScaled = 0;
for k = 1:numel(cases)
    c = cases{k}; n = native.cases(k);
    assert(strcmp(n.id,c.id),'Case ordering differs');
    if c.reject
        % Independently establish the rank loss behind the contract rejection.
        rankMatrix = [];
        for u = 1:c.users
            if c.direction == "dl", rankMatrix = [rankMatrix;c.H{u}(:,:,1)]; %#ok<AGROW>
            else, rankMatrix = [rankMatrix,c.H{u}(:,:,1)]; end %#ok<AGROW>
        end
        assert(rank(rankMatrix) < c.users,'Rejection fixture must have rank loss');
        assert(strcmp(n.status,'rejected'),'Expected rejection: %s',c.id);
        rejected = rejected + 1;
        references(end+1) = struct('id',c.id,'status','rejected', ...
            'precoder',[],'rows',[]); %#ok<AGROW>
        details(end+1) = struct('id',c.id,'direction',c.direction,'method',c.method, ...
            'status','rejected_as_expected','rows',0,'comparisons',0, ...
            'max_absolute_error',0,'max_scaled_error',0); %#ok<AGROW>
        continue;
    end
    assert(strcmp(n.status,'ok'),'Unexpected native rejection: %s',c.id);
    [expected,precoder] = reference(c);
    references(end+1) = struct('id',c.id,'status','ok', ...
        'precoder',[real(precoder),imag(precoder)],'rows',expected); %#ok<AGROW>
    actual = n.rows;
    assert(isequal(size(actual),size(expected)) && ...
        isequal(actual(:,1:3),expected(:,1:3)),'Row identities differ: %s',c.id);
    delta = abs(actual(:,4:end)-expected(:,4:end));
    scale = absoluteTolerance + relativeTolerance*abs(expected(:,4:end));
    errors = delta(:); scaled = delta(:)./scale(:);
    sinrDelta = abs(actual(:,6)-expected(:,6));
    sinrMaxError = max(sinrMaxError,max(sinrDelta));
    sinrMaxScaled = max(sinrMaxScaled,max(sinrDelta./ ...
        (absoluteTolerance+relativeTolerance*abs(expected(:,6)))));
    if ~isempty(precoder)
        nativeW = complex(n.precoder(:,1),n.precoder(:,2));
        assert(isequal(size(nativeW),size(precoder)),'Precoder shape differs');
        errors = [errors;abs(nativeW-precoder)]; %#ok<AGROW>
        scaled = [scaled;abs(nativeW-precoder)./ ...
            (absoluteTolerance+relativeTolerance*abs(precoder))]; %#ok<AGROW>
    end
    assert(all(isfinite(errors)) && all(scaled <= 1), ...
        'MATLAB/native mismatch: %s, scaled error %.6g',c.id,max(scaled));
    maxError = max(maxError,max(errors)); maxScaled = max(maxScaled,max(scaled));
    comparisons = comparisons + numel(errors); rowCount = rowCount + size(expected,1);
    details(end+1) = struct('id',c.id,'direction',c.direction,'method',c.method, ...
        'status','passed','rows',size(expected,1),'comparisons',numel(errors), ...
        'max_absolute_error',max(errors),'max_scaled_error',max(scaled)); %#ok<AGROW>
end
report = struct('schema_version',1,'status','passed', ...
    'scope','MU-MIMO numerical kernels; no WLAN waveform, EESM or PER validation', ...
    'matlab_version',version,'matlab_release',version('-release'), ...
    'created_utc',char(datetime('now','TimeZone','UTC','Format','yyyy-MM-dd''T''HH:mm:ss''Z''')), ...
    'rng_seed',20261028,'absolute_tolerance',absoluteTolerance, ...
    'relative_tolerance',relativeTolerance,'case_count',numel(cases), ...
    'accepted_cases',numel(cases)-rejected,'expected_rejections',rejected, ...
    'user_tone_stream_rows',rowCount,'scalar_comparisons',comparisons, ...
    'max_absolute_error',maxError,'max_scaled_error',maxScaled, ...
    'sinr_max_absolute_error',sinrMaxError,'sinr_max_scaled_error',sinrMaxScaled, ...
    'cases',details);
writeText(fullfile(outputDirectory,'matlab_reference.json'), ...
    jsonencode(struct('schema_version',1,'cases',references),PrettyPrint=true));
writeText(fullfile(outputDirectory,'report.json'),jsonencode(report,PrettyPrint=true));
fprintf('MATLAB MU-MIMO validation PASSED: %d cases, %d rows, %d comparisons\n', ...
    numel(cases),rowCount,comparisons);
fprintf('Max SINR absolute error %.6g; max scaled error %.6g (limit 1)\n', ...
    sinrMaxError,maxScaled);
end

function cases = makeCases()
cases = {};
% Explicit examples discussed in the implementation review.
for method = ["zf","mmse"]
    c = base('ul',method,1,2,2,1);
    c.H{1} = [1;0]; c.H{2} = [0.5;sqrt(3)/2]; c.snr = [100,1];
    c.id = "ul_near_far_"+method; cases{end+1} = c; %#ok<AGROW>
    c.H{2} = c.H{1}; c.snr = [1,1]; c.reject = method == "zf";
    c.id = "ul_rank_loss_"+method; cases{end+1} = c; %#ok<AGROW>
end
c = base('dl','fixed',2,1,2,1);
c.H = {[1,1],[1,1]}; c.W = eye(2); c.snr = [10,10];
c.id = 'dl_same_ru_5_over_6'; cases{end+1} = c;
c.group = [0,1]; c.id = 'dl_separate_ru_10'; cases{end+1} = c;
for direction = ["dl","ul"]
    methods = ["zf","rzf"];
    if direction == "ul", methods = ["zf","mmse"]; end
    for method = methods
        nt = 2; nr = 1;
        if direction == "ul", nt = 1; nr = 2; end
        c = base(direction,method,nt,nr,2,1);
        if direction == "dl", c.H = {[1,0],[0,1]};
        else, c.H = {[1;0],[0;1]}; end
        c.id = direction+"_orthogonal_"+method; cases{end+1} = c; %#ok<AGROW>
        c.H{2} = c.H{1}; c.reject = true;
        if method == "mmse", c.reject = false; end
        c.id = direction+"_rank_loss_"+method; cases{end+1} = c; %#ok<AGROW>
        c.reject = false;
        if direction == "dl", c.H{2} = [1,1e-3];
        else, c.H{2} = [1;1e-3]; end
        c.id = direction+"_ill_conditioned_"+method; cases{end+1} = c; %#ok<AGROW>
    end
end
% Complex Gaussian, frequency-selective cases, 1--4 users, up to 8 antennas.
% All inputs, including the fixed precoders, are generated in MATLAB.
for trial = 1:40
    users = 1+mod(trial,4); antennas = users+2*mod(trial,3); tones = 1+mod(trial,7);
    for method = ["fixed","zf","rzf"]
        c = base('dl',method,antennas,1,users,tones);
        c.id = "dl_random_"+trial+"_"+method;
        c.snr = 10.^(-2+5*rand(1,users)); c.alpha = 10^(-3+4*rand);
        for u = 1:users, c.H{u} = gaussian(1,antennas,tones); end
        if method == "fixed"
            c.W = gaussian(antennas,users,tones);
            for sc = 1:tones, c.W(:,:,sc) = c.W(:,:,sc)*sqrt(users)/norm(c.W(:,:,sc),'fro'); end
            c.group = mod(0:users-1,2);
        end
        cases{end+1} = c; %#ok<AGROW>
    end
    for method = ["zf","mmse"]
        c = base('ul',method,1,antennas,users,tones);
        c.id = "ul_random_"+trial+"_"+method;
        c.snr = 10.^(-2+5*rand(1,users));
        for u = 1:users, c.H{u} = gaussian(antennas,1,tones); end
        cases{end+1} = c; %#ok<AGROW>
    end
end
% Multi-antenna receivers and two streams per DL user, unequal column power.
for trial = 1:12
    c = base('dl','fixed',4,2,2,5);
    c.id = "dl_multistream_"+trial; c.streamUsers = [2,1,2,1];
    c.streamLocal = [0,0,1,1]; c.snr = [0.1,100];
    c.group = [0,mod(trial,2)]; c.W = gaussian(4,4,5);
    for sc = 1:5, c.W(:,:,sc) = c.W(:,:,sc)*2/norm(c.W(:,:,sc),'fro'); end
    for u = 1:2, c.H{u} = gaussian(2,4,5); end
    cases{end+1} = c; %#ok<AGROW>
end
% Zero channel/power boundaries and an overloaded but well-defined UL MMSE.
for direction = ["dl","ul"]
    method = "fixed"; nt = 2; nr = 1;
    if direction == "ul", method = "mmse"; nt = 1; nr = 2; end
    c = base(direction,method,nt,nr,2,1);
    c.id = direction+"_zero_channel_and_power"; c.snr = [0,10];
    c.H = {zeros(nr,nt),ones(nr,nt)}; c.W = eye(2);
    cases{end+1} = c; %#ok<AGROW>
end
c = base('ul','mmse',1,2,3,3); c.id = 'ul_overloaded_mmse';
for u = 1:3, c.H{u} = gaussian(2,1,3); end
cases{end+1} = c;
end

function c = base(direction,method,nt,nr,users,tones)
c = struct('id',"",'direction',string(direction),'method',string(method), ...
    'nt',nt,'nr',nr,'users',users,'tones',tones,'alpha',0.25, ...
    'snr',10*ones(1,users),'group',zeros(1,users), ...
    'streamUsers',1:users,'streamLocal',zeros(1,users), ...
    'H',{cell(1,users)},'W',[],'reject',false);
end

function z = gaussian(varargin)
z = (randn(varargin{:})+1i*randn(varargin{:}))/sqrt(2);
end

function writeFixtures(path,cases)
f = fopen(path,'w'); assert(f >= 0); cleanup = onCleanup(@() fclose(f));
fprintf(f,'%d\n',numel(cases));
for k = 1:numel(cases)
    c = cases{k}; streams = numel(c.streamUsers);
    fprintf(f,'%s %s %s %d %d %d %d %d %.17g\n',c.id,c.direction,c.method, ...
        c.tones,c.nt,c.nr,c.users,streams,c.alpha);
    fprintf(f,'%.17g %d\n',[c.snr;c.group]);
    fprintf(f,'%d %d\n',[c.streamUsers;c.streamLocal]);
    for u = 1:c.users
        for sc = 1:c.tones
            z = reshape(c.H{u}(:,:,sc).',[],1); % nonconjugating: C++ row-major
            fprintf(f,'%.17g %.17g\n',[real(z),imag(z)].');
        end
    end
    if c.direction == "dl" && c.method == "fixed"
        for sc = 1:c.tones
            z = reshape(c.W(:,:,sc).',[],1);
            fprintf(f,'%.17g %.17g\n',[real(z),imag(z)].');
        end
    end
end
end

function [rows,flatW] = reference(c)
rows = []; flatW = []; streams = numel(c.streamUsers);
for sc = 1:c.tones
    if c.direction == "dl"
        if c.method == "fixed"
            W = c.W(:,:,sc);
        else
            A = zeros(c.users,c.nt);
            for u = 1:c.users, A(u,:) = c.H{u}(:,:,sc); end
            if c.method == "zf"
                [Q,R] = qr(A','econ'); W = Q/R';
            else
                W = A'/(A*A'+c.alpha*eye(c.users));
            end
            W = W*sqrt(streams)/norm(W,'fro');
        end
        flatW = [flatW;reshape(W.',[],1)]; %#ok<AGROW>
        for u = 1:c.users
            E = c.H{u}(:,:,sc)*W;
            active = c.group(c.streamUsers) == c.group(u);
            p = c.snr(u)/sum(abs(W(:,active)).^2,'all');
            covariance = eye(c.nr)+p*(E(:,active)*E(:,active)');
            covariance = (covariance+covariance')/2;
            L = chol(covariance,'lower');
            for col = find(c.streamUsers == u)
                g = L'\(L\E(:,col));
                powers = p*abs(g'*E).^2;
                inter = active & c.streamUsers ~= u;
                others = active; others(col) = false;
                desired = powers(col); iui = sum(powers(inter));
                denominator = sum(powers(others))+real(g'*g);
                sinr = 0;
                if denominator > 0, sinr = desired/denominator; end
                % Independent SINR expression; excludes the desired stream.
                C = eye(c.nr)+p*(E(:,others)*E(:,others)');
                identitySinr = p*real(E(:,col)'*(C\E(:,col)));
                assert(abs(sinr-identitySinr) <= 1e-10+1e-9*abs(identitySinr));
                rows(end+1,:) = [u,sc-1,c.streamLocal(col),desired,iui,identitySinr]; %#ok<AGROW>
            end
        end
    else
        H = zeros(c.nr,c.users);
        for u = 1:c.users, H(:,u) = c.H{u}(:,:,sc); end
        if c.method == "zf"
            [Q,R] = qr(H,'econ'); G = Q/R';
        else
            covariance = eye(c.nr)+H*diag(c.snr)*H';
            covariance = (covariance+covariance')/2;
            L = chol(covariance,'lower'); G = L'\(L\H);
        end
        for u = 1:c.users
            g = G(:,u); others = setdiff(1:c.users,u);
            powers = c.snr.*abs(g'*H).^2;
            desired = powers(u); iui = sum(powers(others)); noise = real(g'*g);
            sinr = 0;
            if iui+noise > 0, sinr = desired/(iui+noise); end
            if c.method == "mmse"
                C = eye(c.nr)+H(:,others)*diag(c.snr(others))*H(:,others)';
                identitySinr = c.snr(u)*real(H(:,u)'*(C\H(:,u)));
                assert(abs(sinr-identitySinr) <= 1e-10+1e-9*abs(identitySinr));
                sinr = identitySinr;
            end
            rows(end+1,:) = [u,sc-1,0,desired,iui,sinr,noise]; %#ok<AGROW>
        end
    end
end
rows = sortrows(rows,[1,2,3]);
end

function quoted = shellQuote(value)
% POSIX shell quoting; paths are data, including spaces and apostrophes.
quoted = "'" + replace(string(value),"'","'"+char(34)+"'"+char(34)+"'") + "'";
end

function writeText(path,value)
f = fopen(path,'w'); assert(f >= 0); cleanup = onCleanup(@() fclose(f));
fprintf(f,'%s\n',value);
end
