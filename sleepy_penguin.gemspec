ENV["VERSION"] or abort "VERSION= must be specified"
manifest = File.readlines('.manifest').map! { |x| x.chomp! }
require 'wrongdoc'
extend Wrongdoc::Gemspec
name, summary, title = readme_metadata

Gem::Specification.new do |s|
  s.name = %q{sleepy_penguin}
  s.version = ENV["VERSION"].dup
  s.homepage = 'http://bogomips.org/sleepy_penguin/'
  s.authors = ["#{name} hackers"]
  s.date = Time.now.utc.strftime('%Y-%m-%d')
  s.description = description
  s.email = %q{sleepy.penguin@librelist.com}
  s.extra_rdoc_files = extra_rdoc_files(manifest)
  s.files = manifest
  s.rdoc_options = rdoc_options
  s.require_paths = %w(lib ext)
  s.rubyforge_project = %q{rainbows}
  s.summary = summary
  s.test_files = Dir['test/test_*.rb']
  s.extensions = %w(ext/sleepy_penguin/extconf.rb)
  s.add_development_dependency('wrongdoc', '~> 1.3')

  # s.license = %w(LGPL) # disabled for compatibility with older RubyGems
end
