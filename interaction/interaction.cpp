#include <punc.h>
#include <dolfin.h>
#include <boost/program_options.hpp>
#include <csignal>

using namespace punc;
namespace po = boost::program_options;

using std::cout;
using std::cerr;
using std::endl;
using std::size_t;
using std::vector;
using std::accumulate;
using std::string;
using std::ifstream;
using std::ofstream;

const char* fname_hist  = "history.dat";
const char* fname_state = "state.dat";
const char* fname_pop   = "population.dat";
const bool override_status_print = true;
const double tol = 1e-10;

bool exit_immediately = true;
void signal_handler(int signum){
    if(exit_immediately){
        exit(signum);
    } else {
        cout << endl;
        cout << "Completing and storing timestep before exiting. ";
        cout << "Press Ctrl+C again to force quit." << endl;
        exit_immediately = true;
    }
}

// Horrible, horrible list of arguments. Hopefully only temporary.
template <size_t dim>
int run(
    Mesh &mesh,
    size_t steps,
    double dt,
    double Bx,
    bool impose_current,
    double imposed_current,
    double imposed_voltage,
    vector<int> npc,
    vector<int> num,
    vector<double> density,
    vector<double> thermal,
    vector<double> vx,
    vector<double> charge,
    vector<double> mass,
    vector<double> kappa,
    vector<double> alpha,
    vector<string> distribution,
    bool binary,
    size_t n_fields,
    size_t n_state,
    bool densities_ema,
    double densities_tau,
    bool fields_end,
    bool state_end,
    bool PE_save)
{

    PhysicalConstants constants;
    double eps0 = constants.eps0;

    // To be moved into Mesh
    // auto facet_vec = exterior_boundaries(mesh.bnd, mesh.ext_bnd_id);

    vector<double> B(dim, 0); // Magnetic field aligned with x-axis
    B[0] = Bx;
    double B_norm = accumulate(B.begin(), B.end(), 0.0);

    //
    // CREATE SPECIES
    //

    // FIXME: Move to input file
    vector<vector<double>> vd(charge.size(), vector<double>(dim));

    vector<std::shared_ptr<Pdf>> pdfs;
    vector<std::shared_ptr<Pdf>> vdfs;

    CreateSpecies create_species(mesh);
    for(size_t s=0; s<charge.size(); s++){

        vd[s][0] = vx[s]; // fill in x-component of velocity vector for each species
        pdfs.push_back(std::make_shared<UniformPosition>(mesh.mesh));

        if(distribution[s]=="maxwellian"){
            vdfs.push_back(std::make_shared<Maxwellian>(thermal[s], vd[s]));
        } else if (distribution[s]=="kappa") {
            vdfs.push_back(std::make_shared<Kappa>(thermal[s], vd[s], kappa[s]));
        }else if (distribution[s] == "cairns"){
            vdfs.push_back(std::make_shared<Cairns>(thermal[s], vd[s], alpha[s]));
        }else if (distribution[s] == "kappa-cairns"){
            vdfs.push_back(std::make_shared<KappaCairns>(thermal[s], vd[s], kappa[s], alpha[s]));
        } else {
            cout << "Unsupported velocity distribution: ";
            cout << distribution[s] << endl;
            return 1;
        }

        create_species.create_raw(charge[s], mass[s], density[s],
                *(pdfs[s]), *(vdfs[s]), npc[s], num[s]);
    }
    auto species = create_species.species;

    //
    // IMPOSE CIRCUITRY
    //
    vector<vector<int>> isources, vsources;
    vector<double> ivalues, vvalues;

    if(impose_current){

        isources = {{-1,0}};
        ivalues = {-imposed_current};

        vsources = {};
        vvalues = {};

    } else {

        isources = {};
        ivalues = {};

        vsources = {{-1,0}};
        vvalues = {imposed_voltage};
    }

    //
    // CREATE FUNCTION SPACES AND BOUNDARY CONDITIONS
    //
    auto V = function_space(mesh.mesh);
    auto Q = DG0_space(mesh.mesh);
    auto dv_inv = element_volume(V);

    // Electron and ion number densities
    df::Function ne(std::make_shared<const df::FunctionSpace>(V));
    df::Function ni(std::make_shared<const df::FunctionSpace>(V));
    // Exponential moving average of number densities
    df::Function ne_ema(std::make_shared<const df::FunctionSpace>(V));
    df::Function ni_ema(std::make_shared<const df::FunctionSpace>(V));

    auto u0 = std::make_shared<df::Constant>(0.0);

    // mesh.ext_bnd_id will always be 1, but better not rely on it.
    // Perhaps we can use a function which returns this DirichletBC.
    df::DirichletBC bc(std::make_shared<df::FunctionSpace>(V), u0,
        std::make_shared<df::MeshFunction<size_t>>(mesh.bnd), mesh.ext_bnd_id);
    vector<df::DirichletBC> ext_bc = {bc};

    ObjectBC object(V, mesh.bnd, 2, eps0);
    vector<ObjectBC> int_bc = {object};

    Circuit circuit(V, int_bc, isources, ivalues, vsources, vvalues, dt, eps0);

    //
    // CREATE SOLVERS
    //
    PoissonSolver poisson(V, ext_bc, circuit, eps0);
    ESolver esolver(V);

    //
    // CREATE FLUX
    //
    cout << "Create flux" << endl;
    create_flux(species, mesh.exterior_facets);

    //
    // LOAD NEW PARTICLES OR CONTINUE SIMULATION FROM FILE
    //
    cout << "Loading particles" << endl;

    Population<dim> pop(mesh);

    size_t n = 0;
    double t = 0;
    bool continue_simulation = false;

    ifstream ifile_state(fname_state);
    ifstream ifile_hist(fname_hist);
    ifstream ifile_pop(fname_pop);

    if(ifile_state.good() && ifile_hist.good() && ifile_pop.good()){
        continue_simulation = true;
    }

    ifile_hist.close();
    ifile_pop.close();

    //
    // HISTORY AND SATE FILES
    //
    History hist(fname_hist, int_bc, continue_simulation);
    State state(fname_state);
    FieldWriter fields("Fields/phi.pvd", "Fields/E.pvd", "Fields/rho.pvd", "Fields/ne.pvd", "Fields/ni.pvd");

    if(continue_simulation){
        cout << "Continuing previous simulation" << endl;
        state.load(n, t, int_bc);
        pop.load_file(fname_pop, binary);
    } else {
        cout << "Starting new simulation" << endl;
        load_particles(pop, species);
    }

    //
    // CREATE HISTORY VARIABLES
    //
    double KE               = 0;
    double PE               = 0;
    double num_e            = pop.num_of_negatives();
    double num_i            = pop.num_of_positives();
    double num_tot          = pop.num_of_particles();

    cout << "imposed_current: " << imposed_current <<'\n';
    cout << "imposed_voltage: " << imposed_voltage << '\n';

    cout << "Num positives:  " << num_i;
    cout << ", num negatives: " << num_e;
    cout << " total: " << num_tot << endl;

    //
    // CREATE TIMER TASKS
    //
    vector<string> tasks{"distributor", "poisson", "efield", "update", "PE", "accelerator", "move", "injector", "counting particles", "io", "density"};
    Timer timer(tasks);

    exit_immediately = false;
    auto n_previous = n; // n from previous simulation

    for(; n<=steps; ++n){

        // We are now at timestep n
        // Velocities and currents are at timestep n-0.5 (or 0 if n==0)

        // Print status
        timer.progress(n, steps, n_previous, override_status_print);

        // DISTRIBUTE
        timer.tic("distributor");
        auto rho = distribute_cg1(V, pop, dv_inv);
        timer.toc();

        // SOLVE POISSON
        // reset_objects(int_bc);
        // t_rsetobj[n]= timer.elapsed();
        /* int_bc[0].charge = 0; */
        /* int_bc[0].charge -= current_collected*dt; */
        timer.tic("poisson");
        auto phi = poisson.solve(rho, int_bc, circuit, V);
        timer.toc();

        // UPDATE OBJECT CHARGE AND POTENTIAL
        for(auto& o: int_bc)
        {
            o.update_charge(phi);
            o.update_potential(phi);
        } 
        
        // ELECTRIC FIELD
        timer.tic("efield");
        auto E = esolver.solve(phi);
        timer.toc();

        // compute_object_potentials(int_bc, E, inv_capacity, mesh.mesh);
        //
        // potential[n] = int_bc[0].potential;
        // auto phi1 = poisson.solve(rho, int_bc);
        //
        // auto E1 = esolver.solve(phi1);

        // POTENTIAL ENERGY
        timer.tic("PE");
        if (PE_save)
        {
            PE = particle_potential_energy_cg1(pop, phi);
        }
        timer.toc();

        // COUNT PARTICLES
        timer.tic("counting particles");
        num_e     = pop.num_of_negatives();
        num_i     = pop.num_of_positives();
        num_tot   = pop.num_of_particles();
        timer.toc();

        // PUSH PARTICLES AND CALCULATE THE KINETIC ENERGY
        // Advancing velocities to n+0.5
        timer.tic("accelerator");
        if (fabs(B_norm)<tol)
        {
            KE = accel_cg1(pop, E, (1.0 - 0.5 * (n == 0)) * dt);
        }else{
            KE = boris_cg1(pop, E, B, (1.0 - 0.5 * (n == 0)) * dt);
        }
        if(n==0) KE = kinetic_energy(pop);
        timer.toc();

        // WRITE HISTORY
        // Everything at n, except currents which are at n-0.5.
        timer.tic("io");
        hist.save(n, t, num_e, num_i, KE, PE, int_bc);
        timer.toc();

        // MOVE PARTICLES
        // Advancing position to n+1
        timer.tic("move");
        move(pop, dt);
        timer.toc();

        t += dt;

        // UPDATE PARTICLE POSITIONS
        timer.tic("update");
        pop.update(int_bc);
        timer.toc();

        // CALCULATE COLLECTED CURRENT BY EACH OBJECT
        for (auto &o : int_bc)
        {
            o.update_current(dt);
        }

        // INJECT PARTICLES
        timer.tic("injector");
        inject_particles(pop, species, mesh.exterior_facets, dt);
        timer.toc();

        // AVERAGING
        timer.tic("io");
        if (densities_ema)
        {
            density_cg1(V, pop, ne, ni, dv_inv);
            ema(ne, ne_ema, dt, densities_tau);
            ema(ni, ni_ema, dt, densities_tau);
        }
        // SAVE FIELDS
        if(n_fields !=0 && n%n_fields == 0)
        {
            if (densities_ema)
            {
                fields.save(phi, E, rho, ne_ema, ni_ema, t);
            }else{
                density_cg1(V, pop, ne, ni, dv_inv);
                fields.save(phi, E, rho, ne, ni, t);
            }
        } 
        // SAVE STATE AND BREAK LOOP
        if(exit_immediately || n==steps)
        {
            // SAVE FIELDS
            if (fields_end)
            {
                if (densities_ema)
                {
                    fields.save(phi, E, rho, ne_ema, ni_ema, t);
                }
                else
                {
                    density_cg1(V, pop, ne, ni, dv_inv);
                    fields.save(phi, E, rho, ne, ni, t);
                }
            }
            // SAVE POPULATION AND STATE
            if (state_end)
            {
                pop.save_file(fname_pop, binary);
                state.save(n, t, int_bc);
            }
            break;
        }
        timer.toc();

        if (exit_immediately)
        {
            timer.summary();
        }
    }
    if(override_status_print) cout << endl;

    // Print a summary of tasks
    timer.summary();
    cout << "PUNC++ finished successfully!" << endl;
    return 0;
}

int main(int argc, char **argv){

    signal(SIGINT, signal_handler);
    df::set_log_level(df::WARNING);

    //
    // INPUT VARIABLES
    //

    // Global input
    string fname_ifile;
    string fname_mesh;
    size_t steps = 0;
    double dt = 0;
    double dtwp;
    double Bx = 0;
    bool binary = false;

    // diagnostics input
    size_t n_fields = 0;
    size_t n_state = 0;
    bool densities_ema = false;
    double densities_tau = 0.0;
    bool fields_end = false;
    bool state_end = true;
    bool PE_save = false;

    // Object input
    bool impose_current = true; 
    double imposed_current;
    double imposed_voltage;

    // Species input
    vector<int> npc;
    vector<int> num;
    vector<double> density;
    vector<double> thermal;
    vector<double> vx;
    vector<double> charge;
    vector<double> mass;
    vector<double> kappa;
    vector<double> alpha;
    vector<string> distribution;

    po::options_description desc("Options");
    desc.add_options()
        ("help", "show help (this)")
        ("input", po::value(&fname_ifile), "config file")
        ("mesh", po::value(&fname_mesh), "mesh file")
        ("steps", po::value(&steps), "number of timesteps")
        ("dt", po::value(&dt), "timestep [s] (overrides dtwp)") 
        ("dtwp", po::value(&dtwp), "timestep [1/w_p of first specie]")
        ("binary", po::value(&binary), "Write binary population files (true|false)")
        
        ("Bx", po::value(&Bx), "magnetic field [T]")

        ("impose_current", po::value(&impose_current), "Whether to impose current or voltage (true|false)")
        ("imposed_current", po::value(&imposed_current), "Current imposed on object [A]")
        ("imposed_voltage", po::value(&imposed_voltage), "Voltage imposed on object [V]")

        ("species.charge", po::value(&charge), "charge [elementary chages]")
        ("species.mass", po::value(&mass), "mass [electron masses]")
        ("species.density", po::value(&density), "number density [1/m^3]")
        ("species.thermal", po::value(&thermal), "thermal speed [m/s]")
        ("species.vx", po::value(&vx), "drift velocity [m/s]")
        ("species.alpha", po::value(&alpha), "spectral index alpha")
        ("species.kappa", po::value(&kappa), "spectral index kappa")
        ("species.npc", po::value(&npc), "number of particles per cell")
        ("species.num", po::value(&num), "number of particles in total (overrides npc)")
        ("species.distribution", po::value(&distribution), "distribution (maxwellian)")

        ("diagnostics.n_fields", po::value(&n_fields), "write fields to file every nth time-step")
        ("diagnostics.n_state", po::value(&n_state), "write state to file every nth time-step")
        ("diagnostics.densities_ema", po::value(&densities_ema), "apply exponential moving average on densities")
        ("diagnostics.densities_tau", po::value(&densities_tau), "relaxation time")
        ("diagnostics.fields_end", po::value(&fields_end), "write to file every nth time-step")
        ("diagnostics.state_end", po::value(&state_end), "write to file every nth time-step")
        ("diagnostics.PE_save", po::value(&PE_save), "calculate and save potential energy")
    ;

    // Setting config file as positional argument
    po::positional_options_description pos_options;
    pos_options.add("input", -1);

    //
    // PARSING INPUT
    //

    // Parse input from command line first, including name of config file.
    // These settings takes precedence over input from config file.
    po::variables_map options;
    po::store(po::command_line_parser(argc, argv).
              options(desc).positional(pos_options).run(), options);
    po::notify(options);

    // Parse input from config file.
    if(options.count("input")){
        ifstream ifile;
        ifile.open(fname_ifile);
        po::store(po::parse_config_file(ifile, desc), options);
        po::notify(options);
        ifile.close();
    } else {
        cerr << "Input file missing." << endl;
        return 1;
    }

    // Print help
    if(options.count("help")){
        cout << desc << endl;
        return 1;
    }

    cout << "PUNC++ started!" << endl;

    //
    // PRE-PROCESS INPUT
    //
    PhysicalConstants constants;
    double eps0 = constants.eps0;

    size_t nSpecies = charge.size();
    for(size_t s=0; s<nSpecies; s++){
        charge[s] *= constants.e;
        mass[s] *= constants.m_e;
    }

    if(fabs(dt)<tol){
        double wp0 = sqrt(pow(charge[0],2)*density[0]/(eps0*mass[0]));
        dt = dtwp/wp0;
    }

    if(num.size() != 0 && npc.size() != 0){
        cout << "Use only npc or num. Not mixed." << endl;
        return 1;
    }
    if(num.size() == 0) num = vector<int>(nSpecies, 0);
    if(npc.size() == 0) npc = vector<int>(nSpecies, 0);
    if(kappa.size() == 0) kappa = vector<double>(nSpecies, 0);
    if(alpha.size() == 0) alpha = vector<double>(nSpecies, 0);
    if(vx.size() == 0) vx = vector<double>(nSpecies, 0);

    // Sanity checks (avoids segfaults)
    if(charge.size()       != nSpecies
    || mass.size()         != nSpecies
    || density.size()      != nSpecies
    || distribution.size() != nSpecies 
    || npc.size()          != nSpecies
    || num.size()          != nSpecies
    || thermal.size()      != nSpecies
    || vx.size()           != nSpecies
    || kappa.size()        != nSpecies
    || alpha.size()        != nSpecies){

        cout << "Wrong arguments for species." << endl;
        return 1;
    }

    //
    // CREATE MESH
    //
    Mesh mesh(fname_mesh);

    // TBD: dim==1?
    if(mesh.dim==2){
        return run<2>(mesh, steps, dt, Bx, impose_current, imposed_current, imposed_voltage, npc, num, density, thermal, vx, charge, mass, kappa, alpha, distribution, binary, n_fields, n_state, densities_ema, densities_tau, fields_end, state_end, PE_save);
    } else if(mesh.dim==3){
        return run<3>(mesh, steps, dt, Bx, impose_current, imposed_current, imposed_voltage, npc, num, density, thermal, vx, charge, mass, kappa, alpha, distribution, binary, n_fields, n_state, densities_ema, densities_tau, fields_end, state_end, PE_save);
    } else {
        cout << "Only 2D and 3D supported" << endl;
        return 1;
    }
}
