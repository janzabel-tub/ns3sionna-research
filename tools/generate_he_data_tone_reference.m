function generate_he_data_tone_reference(outputPath)
% All physical HE data/pilot masks; no simulation or fitted parameters.
rows = struct('width',{},'size',{},'index',{},'active',{},'pilots',{},'data',{});
for w = [20 40 80 160]
 for sz = [26 52 106 242 484 996 1992]
  for idx = 1:80
   try
    inf = wlanHEOFDMInfo('HE-Data',sprintf('CBW%d',w),0.8,[sz idx]);
   catch
    break
   end
   rows(end+1) = struct('width',w,'size',sz,'index',idx,'active',inf.ActiveFrequencyIndices','pilots',inf.ActiveFrequencyIndices(inf.PilotIndices)','data',inf.ActiveFrequencyIndices(inf.DataIndices)');
  end
 end
end
f = fopen(outputPath,'w'); fwrite(f,jsonencode(struct('source','MATLAB WLAN Toolbox R2026a wlanHEOFDMInfo HE-Data, GI 0.8 us','rows',rows))); fclose(f);
disp(numel(rows));

end
