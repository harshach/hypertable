require 'rubygems'
require 'yaml'
require 'json'
require 'sinatra/base'
%w(helpers).each { |r| require "#{ File.dirname(__FILE__)}/app/#{r}"}
#Dir["app/lib/*.rb"].each {|r| require r}
Dir["app/lib/data/*.rb"].each {|r| require r}
# all the files under
%w(base).each { |r| require "#{ File.dirname(__FILE__)}/app/#{r}"}
