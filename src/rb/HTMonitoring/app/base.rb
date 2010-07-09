require 'titleize'

module HTMonitoring
  def self.config
    @config ||= YAML.load_file("config/config.yml")[Sinatra::Application.environment]
  end

  class Admin < Sinatra::Base
    enable :static
    set :public, File.join(File.dirname(__FILE__), '..', 'public')
    set :environment, :development
    set :raise_errors, true

    helpers do
      include HTMonitoring::Helpers
    end
    # this is the home page handler which show overview
    get '/' do
      @table_stats = TableStats.new
      @table_stats.get_stats_totals

      @range_servers = RangeServerStats.new
      @range_servers.get_stats_totals

      erb :index
    end

    get '/tables' do
      erb :tables
    end

    get '/rangeservers' do
      erb :rangeservers
    end

    get '/test' do
      erb :test
    end


   get %r{/data/([^/]+)/([^/]+)/([^/]+)} do
      type = params[:captures][0]
      stat = params[:captures][1]
      time_interval = params[:captures][2]
      if type.downcase == "table"
        stats = TableStats.new
        json = stats.get_graph_data({:stat => stat, :timestamp_index => time_interval.to_i})
        maybe_wrap_with_callback(json)
      elsif type.downcase == "rangeserver"
        stats = RRDStat.new
        json = stats.get_graph_data({:stat => stat, :timestamp_index => time_interval.to_i})
        maybe_wrap_with_callback(json)
      end
   end



    # wraps json with a callback method that JSONP clients can call
    def maybe_wrap_with_callback(json)
      params[:callback] ? params[:callback] + '(' + json + ')' : json
    end

  end

end
