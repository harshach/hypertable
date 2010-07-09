# This class is responsible for reading table_stats and rs_stats file
# still keeping lot of old code.
# removing google charts stuff and adding methods to expose data as json.
# This class handles retrieving stats for both table and range servers
# The code is not clean its a work in progress
# should make it into a module
# Author : Sriharsha Chintalapani(harsha@defun.org)

class StatsParser
  attr_accessor :stats_list

  def initialize(opts={ })
    @datadir = opts[:datadir]
    @stats_file = @datadir+opts[:stats_file]
    @copy_stats_file = @datadir+"copy_"+opts[:stats_file]
    @stat_types = opts[:stat_types]
    @parser = opts[:parser]
  end



  def parse_stats_file(wait_time=2)
    return false unless File.exists?(@stats_file)
    copy_stats_file(wait_time)
    stats_list = []
    #parse copied file here
    file = File.open("#{@copy_stats_file}", "r")
    current_table = '' # to keep track current table parsing
    file.each do |line|
      if line =~ /^(#{@parser}).*=\s*(\w+)/
        current_table = Stat.new($2) # assuming $2 as id
        stats_list.push(current_table)
        #list.push current_stat
      elsif line =~ /^\t(.+)=(.+)/
        #please look into util.rb under helpers we have disabled some keys due to invalid stats
        # timestamps should be parsed and they are not part of stat_types
        next if $1 != "Timestamps" and !@stat_types.include?($1)
        key = :"#{$1}"
        values = $2.split(",")

        values.map! do |v|
          if v =~ /\./
            v.to_f  #data can be floats
          else
            v.to_i
          end
        end
        # loop through all the values and put each it in apporiate index in table's stat
        values.each_with_index do |value, index|
          current_table.stats[index] ||= { } # creates a nested hash
          current_table.stats[index][:"#{key}"] = value
        end

      end

    end
    file.close
    return stats_list

  end


  # gets the time intervals , we only need to look at the first table object
  # need to do it better way ?
  def copy_stats_file(wait_time=2)
    # repeats the copy for some given time.
    time_spent = 0
    start_time = Time.now
    elapsed_time = Time.now
    # not sure why its need to copy for the given time
    begin
      elapsed_time = Time.now
      FileUtils.copy("#{@stats_file}", "#{@copy_stats_file}")
    rescue => err
      time_spent = elapsed_time - start_time
      if time_spent <= wait_time
        retry
      else
        #use old file if possible
        if File.exists?("#{@copy_stats_file}")
          using_new_file = false
        else
          raise
        end
      end
    end
  end


end
