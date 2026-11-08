function ber = he_gray_ber_reference(gamma,mcs)
%HE_GRAY_BER_REFERENCE Independent AWGN decision-region integration.
% gamma is Es/N0, not Eb/N0. Gray labels and constellation coordinates come
% from Communications Toolbox qammod; no C++ QAM-series coefficients are used.
sizes = [2 4 4 16 16 64 64 64 256 256 1024 1024];
M = sizes(mcs+1);
if M == 2, ber = 0.5*erfc(sqrt(gamma)); return; end
persistent regions
if isempty(regions), regions = cell(1,12); end
if isempty(regions{mcs+1})
    labels = (0:M-1)';
    constellation = qammod(labels,M,'gray',UnitAveragePower=true);
    chosen = imag(constellation) == imag(constellation(1));
    [levels,order] = sort(real(constellation(chosen)));
    labels = labels(chosen); labels = uint32(labels(order));
    mid = (levels(1:end-1)+levels(2:end))/2;
    lower = [-Inf;mid].'-levels; upper = [mid;Inf].'-levels;
    differences = bitxor(labels,labels.');
    weights = zeros(size(differences));
    for bit = 1:log2(M), weights = weights+double(bitget(differences,bit)); end
    regions{mcs+1} = struct('lower',lower,'upper',upper,'weights',weights, ...
        'denominator',numel(levels)*log2(M)/2);
end
r = regions{mcs+1}; ber = zeros(size(gamma));
for k = 1:numel(gamma)
    if gamma(k) == 0, ber(k) = 0.5; continue; end
    % Use the nearer Gaussian tail to avoid subtracting two rounded ones.
    a = r.lower*sqrt(gamma(k)); b = r.upper*sqrt(gamma(k));
    probability = 0.5*(erfc(a)-erfc(b));
    left = r.lower < 0;
    probability(left) = 0.5*(erfc(-b(left))-erfc(-a(left)));
    ber(k) = sum(probability.*r.weights,'all')/r.denominator;
end
end
